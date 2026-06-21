#include "nyx/rendezvous_pool.hpp"

#include "nyx/nat.hpp"
#include "nyx/util.hpp"

#include <chrono>

namespace nyx {

RendezvousPool::RendezvousPool(UdpSocket socket) : socket_(std::move(socket)) {}

void RendezvousPool::set_servers(const std::vector<RendezvousServer>& servers) {
  servers_ = servers;
}

bool RendezvousPool::send_to_server(const RendezvousServer& server, PacketType type,
                                    const ByteBuffer& payload) {
  auto wire = Frame::make(type, 0, 0, payload).encode();
  return socket_.send_to(wire, server.host, server.port);
}

bool RendezvousPool::register_token(const InviteToken& token) {
  if (servers_.empty()) return false;
  RendezvousMessage msg;
  msg.kind = RendezvousKind::Register;
  msg.token = token;
  msg.hint = make_public_hint(socket_, guess_lan_ipv4(), socket_.local_port());
  const auto payload = msg.encode();
  bool any = false;
  for (const auto& srv : servers_) {
    if (send_to_server(srv, PacketType::RendezvousRegister, payload)) any = true;
  }
  return any;
}

std::optional<EndpointHint> RendezvousPool::lookup_on(const RendezvousServer& server,
                                                      const InviteToken& token,
                                                      int timeout_ms) {
  RendezvousMessage msg;
  msg.kind = RendezvousKind::Lookup;
  msg.token = token;
  if (!send_to_server(server, PacketType::RendezvousLookup, msg.encode())) {
    return std::nullopt;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string host;
    uint16_t port = 0;
    auto data = socket_.recv_from(host, port, 200);
    if (!data) continue;
    if (!endpoint_matches(host, port, server.host, server.port)) continue;

    auto frame = Frame::decode(data->data(), data->size());
    if (!frame || frame->header.packet_type != PacketType::RendezvousResponse) continue;
    auto resp =
        RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
    if (!resp || resp->kind != RendezvousKind::Response) continue;
    if (resp->hint.port == 0) continue;
    return resp->hint;
  }
  return std::nullopt;
}

std::optional<EndpointHint> RendezvousPool::lookup(const InviteToken& token) {
  for (const auto& srv : servers_) {
    if (auto hint = lookup_on(srv, token, 4000)) return hint;
  }
  return std::nullopt;
}

bool RendezvousPool::probe_server(const RendezvousServer& server, int timeout_ms) {
  InviteToken dummy{};
  random_bytes(dummy.data(), dummy.size());
  RendezvousMessage msg;
  msg.kind = RendezvousKind::Lookup;
  msg.token = dummy;
  if (!send_to_server(server, PacketType::RendezvousLookup, msg.encode())) return false;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string host;
    uint16_t port = 0;
    auto data = socket_.recv_from(host, port, 200);
    if (!data) continue;
    if (!endpoint_matches(host, port, server.host, server.port)) continue;
    auto frame = Frame::decode(data->data(), data->size());
    if (!frame || frame->header.packet_type != PacketType::RendezvousResponse) continue;
    return true;
  }
  return false;
}

}  // namespace nyx
