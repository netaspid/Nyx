#include "nyx/connection.hpp"

#include "nyx/util.hpp"

#include <chrono>
#include <cstring>

namespace nyx {

namespace {

PacketType handshake_reply_type(HandshakeRole role) {
  return role == HandshakeRole::Initiator ? PacketType::HandshakeFinish
                                          : PacketType::HandshakeResp;
}

}  // namespace

Connection::Connection(UdpSocket socket, std::string peer_host, uint16_t peer_port)
    : socket_(std::move(socket)),
      peer_host_(std::move(peer_host)),
      peer_port_(peer_port) {}

bool Connection::send_handshake(PacketType type, const ByteBuffer& payload) {
  auto wire = Frame::make(type, 0, 0, payload).encode();
  return socket_.send_to(wire, peer_host_, peer_port_);
}

bool Connection::run_handshake(HandshakeDriver& hs, const ByteBuffer* first_in) {
  const auto send_step = [&](const ByteBuffer* inbound) -> bool {
    if (auto out = hs.step(inbound)) {
      if (!send_handshake(handshake_reply_type(hs.role()), *out)) return false;
    }
    return true;
  };

  if (hs.role() == HandshakeRole::Initiator) {
    if (auto out = hs.step(nullptr)) {
      if (!send_handshake(PacketType::HandshakeInit, *out)) return false;
    } else {
      return false;
    }
  }

  if (first_in && !first_in->empty()) {
    auto frame = Frame::decode(first_in->data(), first_in->size());
    if (!frame) return false;
    if (!send_step(&frame->payload)) return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (hs.complete()) break;

    std::string host;
    uint16_t port = 0;
    auto pkt = socket_.recv_from(host, port, 100);
    if (!pkt || host != peer_host_ || port != peer_port_) continue;

    auto frame = Frame::decode(pkt->data(), pkt->size());
    if (!frame) continue;
    const auto t = frame->header.packet_type;
    if (t != PacketType::HandshakeInit && t != PacketType::HandshakeResp &&
        t != PacketType::HandshakeFinish) {
      continue;
    }

    if (!send_step(&frame->payload)) return false;
    if (hs.complete()) break;
  }

  if (!hs.complete()) return false;
  auto sess = Session::from_handshake(hs);
  if (!sess) return false;
  session_ = std::move(*sess);
  state_ = ConnectionState::Established;
  const auto now = std::chrono::steady_clock::now();
  last_peer_activity_ = now;
  last_ping_sent_ = now;
  return true;
}

std::optional<Connection> Connection::connect_initiator(UdpSocket socket,
                                                        const std::string& peer_host,
                                                        uint16_t peer_port,
                                                        int timeout_ms) {
  (void)timeout_ms;
  Connection conn(std::move(socket), peer_host, peer_port);
  HandshakeDriver hs(HandshakeRole::Initiator);
  if (!conn.run_handshake(hs, nullptr)) return std::nullopt;
  return conn;
}

std::optional<Connection> Connection::accept_responder(
    UdpSocket socket, const std::string& peer_host, uint16_t peer_port,
    const ByteBuffer* first_packet, int timeout_ms) {
  (void)timeout_ms;
  Connection conn(std::move(socket), peer_host, peer_port);
  HandshakeDriver hs(HandshakeRole::Responder);
  if (!conn.run_handshake(hs, first_packet)) return std::nullopt;
  return conn;
}

bool Connection::send_stream(uint32_t stream_id, const ByteBuffer& data, bool check_rekey) {
  if (!session_) return false;
  auto muxed = mux_.send(stream_id, data);
  auto encrypted = session_->encrypt(muxed);
  if (!encrypted) return false;
  for (auto& wire : reliable_.send(stream_id, *encrypted)) {
    outbound_wires_.push_back(std::move(wire));
  }
  flush_outbound();
  if (check_rekey) maybe_rekey();
  return true;
}

void Connection::flush_outbound() {
  constexpr std::size_t kMaxPerCall = 64;
  std::size_t sent = 0;
  while (!outbound_wires_.empty() && sent++ < kMaxPerCall) {
    if (!socket_.send_to(outbound_wires_.front(), peer_host_, peer_port_)) {
      break;
    }
    outbound_wires_.pop_front();
  }
}

bool Connection::send_text(uint32_t stream_id, const std::string& text) {
  return send_payload(stream_id, ByteBuffer(text.begin(), text.end()));
}

bool Connection::send_payload(uint32_t stream_id, const ByteBuffer& data) {
  return send_stream(stream_id, data);
}

void Connection::touch_peer_activity() {
  last_peer_activity_ = std::chrono::steady_clock::now();
}

void Connection::process_wire(const ByteBuffer& wire) {
  reliable_.recv_wire(wire);
  for (auto& ack : reliable_.make_ack_frames(0)) {
    outbound_wires_.push_back(std::move(ack));
  }
  for (auto& w : reliable_.drain_outbound()) {
    outbound_wires_.push_back(std::move(w));
  }
  flush_outbound();
  touch_peer_activity();
  while (auto data = reliable_.poll_recv()) {
    decrypt_dispatch(*data);
    touch_peer_activity();
  }
}

void Connection::feed_wire(const ByteBuffer& wire) { process_wire(wire); }

bool Connection::pop_stream(uint32_t& stream_id, ByteBuffer& out) {
  for (uint32_t sid : {kChatStream, kBulkStream, 4u, 6u}) {
    if (sid == kControlStream) continue;
    if (auto data = mux_.recv(sid)) {
      stream_id = sid;
      out = std::move(*data);
      return true;
    }
  }
  return false;
}

bool Connection::drive_without_recv() {
  if (state_ != ConnectionState::Established) return false;

  const auto now = std::chrono::steady_clock::now();
  if (now - last_peer_activity_ > std::chrono::seconds(45)) {
    peer_alive_ = false;
    state_ = ConnectionState::Closed;
    return false;
  }
  if (now - last_ping_sent_ > std::chrono::seconds(15)) {
    ping();
    last_ping_sent_ = now;
  }
  for (auto& w : reliable_.drain_outbound()) {
    outbound_wires_.push_back(std::move(w));
  }
  flush_outbound();
  return true;
}

bool Connection::process_incoming(int timeout_ms) {
  if (state_ != ConnectionState::Established || !session_) return false;

  while (auto data = reliable_.poll_recv()) {
    decrypt_dispatch(*data);
    touch_peer_activity();
  }

  std::string host;
  uint16_t port = 0;
  if (timeout_ms == 0) {
    while (auto pkt = socket_.recv_from(host, port, 0)) {
      if (host == peer_host_ && port == peer_port_) {
        process_wire(*pkt);
      }
    }
  } else if (auto pkt = socket_.recv_from(host, port, timeout_ms)) {
    if (host == peer_host_ && port == peer_port_) {
      process_wire(*pkt);
    }
  }
  return true;
}

bool Connection::decrypt_dispatch(const ByteBuffer& cipher) {
  if (!session_) return false;
  auto plain = session_->decrypt(cipher);
  if (!plain || plain->size() < 4) return false;
  const uint32_t stream_id = read_u32_le(plain->data());
  ByteBuffer payload(plain->begin() + 4, plain->end());

  if (stream_id == kControlStream) {
    if (auto msg = ControlMessage::decode(payload.data(), payload.size())) {
      if (msg->kind == ControlKind::Rekey) {
        apply_rekey_control(msg->nonce);
        return true;
      }
    }
    for (auto& reply : mux_.handle_control(payload)) {
      send_stream(kControlStream, reply);
    }
    return true;
  }

  mux_.push(stream_id, std::move(payload));
  return true;
}

bool Connection::send_rekey(std::uint64_t epoch) {
  ControlMessage msg;
  msg.kind = ControlKind::Rekey;
  msg.nonce = epoch;
  if (!send_stream(kControlStream, msg.encode(), false)) return false;
  return session_->perform_rekey(epoch);
}

bool Connection::apply_rekey_control(std::uint64_t epoch) {
  if (!session_) return false;
  return session_->perform_rekey(epoch);
}

void Connection::maybe_rekey() {
  if (!session_ || !session_->needs_rekey()) return;
  send_rekey(session_->rekey_epoch() + 1);
}

bool Connection::recv_stream(uint32_t& stream_id, ByteBuffer& out) {
  process_incoming(0);

  for (uint32_t sid : {kChatStream, kBulkStream, 4u, 6u}) {
    if (sid == kControlStream) continue;
    if (auto data = mux_.recv(sid)) {
      stream_id = sid;
      out = std::move(*data);
      return true;
    }
  }
  return false;
}

bool Connection::ping() { return send_stream(kControlStream, mux_.ping()); }

std::uint64_t Connection::session_rekey_epoch() const {
  return session_ ? session_->rekey_epoch() : 0;
}

bool Connection::drive() {
  if (state_ != ConnectionState::Established) return false;

  process_incoming(0);

  const auto now = std::chrono::steady_clock::now();
  if (now - last_peer_activity_ > std::chrono::seconds(45)) {
    peer_alive_ = false;
    state_ = ConnectionState::Closed;
    return false;
  }
  if (now - last_ping_sent_ > std::chrono::seconds(15)) {
    ping();
    last_ping_sent_ = now;
  }
  for (auto& wire : reliable_.drain_outbound()) {
    outbound_wires_.push_back(std::move(wire));
  }
  flush_outbound();
  maybe_rekey();
  return true;
}

}  // namespace nyx
