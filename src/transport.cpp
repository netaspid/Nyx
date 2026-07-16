#include "nyx/transport.hpp"

#include "nyx/proto.hpp"
#include "nyx/util.hpp"

#include <algorithm>

namespace nyx {

namespace {

bool seq_less(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}

ByteBuffer encode_ack(uint32_t ack, const std::vector<uint32_t>& sack) {
  ByteBuffer out;
  write_u32_le(out, ack);
  write_u16_le(out, static_cast<uint16_t>(sack.size()));
  for (uint32_t s : sack) write_u32_le(out, s);
  return out;
}

void parse_ack(const ByteBuffer& payload, uint32_t& ack,
               std::vector<uint32_t>& sack) {
  if (payload.size() < 6) return;
  ack = read_u32_le(payload.data());
  uint16_t n = read_u16_le(payload.data() + 4);
  std::size_t off = 6;
  for (uint16_t i = 0; i < n && off + 4 <= payload.size(); ++i) {
    sack.push_back(read_u32_le(payload.data() + off));
    off += 4;
  }
}

}  // namespace

ReliableSession::ReliableSession(std::size_t window, std::size_t mtu)
    : mtu_(mtu), window_(window) {}

std::vector<ByteBuffer> ReliableSession::fragment(uint32_t msg_id,
                                                   const ByteBuffer& data) {
  constexpr std::size_t kFragHdr = 8;
  if (mtu_ <= kFragHdr) return {};
  const std::size_t chunk = mtu_ - kFragHdr;
  const std::size_t need =
      std::max<std::size_t>(1, (data.size() + chunk - 1) / chunk);
  // total в заголовке — uint16_t; переполнение ломает сборку (UB/порча кучи).
  if (need > 65535) return {};
  const uint16_t total = static_cast<uint16_t>(need);
  std::vector<ByteBuffer> out;
  for (std::size_t idx = 0, off = 0; off < data.size() || (idx == 0 && data.empty());
       ++idx, off += chunk) {
    const std::size_t n = std::min(chunk, data.size() - off);
    ByteBuffer frag;
    write_u32_le(frag, msg_id);
    write_u16_le(frag, static_cast<uint16_t>(idx));
    write_u16_le(frag, total);
    frag.insert(frag.end(), data.begin() + off, data.begin() + off + n);
    out.push_back(std::move(frag));
    if (off + n >= data.size()) break;
  }
  return out;
}

std::optional<ReliableSession::AssembledMessage> ReliableSession::assemble(
    const ByteBuffer& chunk) {
  if (chunk.size() < 8) return AssembledMessage{0, chunk};
  const uint32_t msg_id = read_u32_le(chunk.data());
  const uint16_t idx = read_u16_le(chunk.data() + 4);
  const uint16_t total = read_u16_le(chunk.data() + 6);
  if (total == 0) return std::nullopt;
  if (total <= 1) {
    return AssembledMessage{msg_id, ByteBuffer(chunk.begin() + 8, chunk.end())};
  }
  auto& p = partials_[msg_id];
  if (p.total == 0) {
    p.total = total;
    p.parts.assign(total, std::nullopt);
  } else if (p.total != total) {
    // Конфликт заголовков — сброс.
    partials_.erase(msg_id);
    return std::nullopt;
  }
  if (idx < p.parts.size() && !p.parts[idx]) {
    p.parts[idx] = ByteBuffer(chunk.begin() + 8, chunk.end());
    ++p.got;
  }
  if (p.got < p.total) return std::nullopt;
  ByteBuffer out;
  for (const auto& part : p.parts) {
    if (!part) {
      partials_.erase(msg_id);
      return std::nullopt;
    }
    out.insert(out.end(), part->begin(), part->end());
  }
  partials_.erase(msg_id);
  return AssembledMessage{msg_id, std::move(out)};
}

void ReliableSession::enqueue_recv(uint32_t msg_id, ByteBuffer data) {
  if (msg_id < recv_msg_next_) return;
  recv_hold_[msg_id] = std::move(data);
  while (recv_hold_.count(recv_msg_next_)) {
    recv_queue_.push_back(std::move(recv_hold_[recv_msg_next_]));
    recv_hold_.erase(recv_msg_next_++);
  }
}

std::optional<ByteBuffer> ReliableSession::encode_data(
    uint32_t stream_id, uint32_t seq, const ByteBuffer& payload) const {
  return Frame::make(PacketType::Data, stream_id, seq, payload).encode();
}

std::vector<ByteBuffer> ReliableSession::flush_pending() {
  std::vector<ByteBuffer> frames;
  constexpr std::size_t kMaxBatch = 32;
  std::size_t batch = 0;
  while (!pending_.empty() && batch < kMaxBatch) {
    auto& ps = pending_.front();
    while (ps.next < ps.frags.size() && inflight_.size() < window_ && batch < kMaxBatch) {
      const uint32_t seq = next_seq_++;
      inflight_[seq] = SendItem{ps.frags[ps.next], 0};
      if (auto wire = encode_data(ps.stream_id, seq, ps.frags[ps.next])) {
        frames.push_back(std::move(*wire));
      }
      ++ps.next;
      ++batch;
    }
    if (ps.next >= ps.frags.size()) {
      pending_.erase(pending_.begin());
    } else {
      break;
    }
  }
  return frames;
}

std::vector<ByteBuffer> ReliableSession::drain_outbound() { return flush_pending(); }

std::vector<ByteBuffer> ReliableSession::send(uint32_t stream_id,
                                               const ByteBuffer& data) {
  PendingSend ps;
  ps.stream_id = stream_id;
  ps.frags = fragment(next_msg_id_++, data);
  pending_.push_back(std::move(ps));
  return flush_pending();
}

void ReliableSession::on_ack(uint32_t ack, const std::vector<uint32_t>& sack) {
  std::vector<uint32_t> remove;
  for (const auto& [seq, item] : inflight_) {
    if (seq_less(seq, ack + 1) || std::find(sack.begin(), sack.end(), seq) != sack.end()) {
      remove.push_back(seq);
    }
  }
  for (uint32_t seq : remove) inflight_.erase(seq);
  while (seq_less(send_base_, next_seq_) && !inflight_.count(send_base_)) {
    ++send_base_;
  }
}

std::vector<ByteBuffer> ReliableSession::on_data(uint32_t seq,
                                                  const ByteBuffer& payload) {
  std::vector<ByteBuffer> delivered;
  if (seq_less(seq, recv_base_)) return delivered;
  recv_buf_[seq] = payload;
  while (recv_buf_.count(recv_base_)) {
    delivered.push_back(recv_buf_[recv_base_]);
    recv_buf_.erase(recv_base_);
    ++recv_base_;
  }
  return delivered;
}

void ReliableSession::recv_wire(const ByteBuffer& wire) {
  auto frame = Frame::decode(wire.data(), wire.size());
  if (!frame) return;

  if (frame->header.packet_type == PacketType::Data) {
    auto chunks = on_data(frame->header.seq_num, frame->payload);
    for (const auto& raw : chunks) {
      if (auto assembled = assemble(raw)) {
        enqueue_recv(assembled->msg_id, std::move(assembled->data));
      }
    }
    std::vector<uint32_t> sack;
    for (const auto& [seq, _] : recv_buf_) {
      if (seq_less(recv_base_, seq)) sack.push_back(seq);
    }
    on_ack(recv_base_ == 0 ? 0 : recv_base_ - 1, sack);
  } else if (frame->header.packet_type == PacketType::Ack) {
    uint32_t ack = 0;
    std::vector<uint32_t> sack;
    parse_ack(frame->payload, ack, sack);
    on_ack(ack, sack);
  }
}

std::optional<ByteBuffer> ReliableSession::poll_recv() {
  if (recv_queue_.empty()) return std::nullopt;
  ByteBuffer out = std::move(recv_queue_.front());
  recv_queue_.erase(recv_queue_.begin());
  return out;
}

std::vector<ByteBuffer> ReliableSession::make_ack_frames(uint32_t stream_id) const {
  std::vector<uint32_t> sack;
  for (const auto& [seq, _] : recv_buf_) {
    if (seq_less(recv_base_, seq)) sack.push_back(seq);
  }
  auto payload = encode_ack(recv_base_ == 0 ? 0 : recv_base_ - 1, sack);
  auto wire = Frame::make(PacketType::Ack, stream_id, 0, payload).encode();
  return {std::move(wire)};
}

}  // namespace nyx
