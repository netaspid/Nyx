#include "nyx/mux.hpp"

#include "nyx/util.hpp"

namespace nyx {

Multiplexer::Multiplexer() {
  streams_[kControlStream] = Stream{StreamType::Control, {}, true};
}

uint32_t Multiplexer::open_stream(StreamType type) {
  const uint32_t id = next_stream_id_;
  next_stream_id_ += 2;
  streams_[id] = Stream{type, {}, true};
  return id;
}

ByteBuffer Multiplexer::send(uint32_t stream_id, const ByteBuffer& data) {
  ByteBuffer out;
  write_u32_le(out, stream_id);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::optional<ByteBuffer> Multiplexer::recv(uint32_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || it->second.queue.empty()) return std::nullopt;
  ByteBuffer out = std::move(it->second.queue.front());
  it->second.queue.pop_front();
  return out;
}

ByteBuffer Multiplexer::ping() {
  ControlMessage msg;
  msg.kind = ControlKind::Ping;
  random_bytes(reinterpret_cast<uint8_t*>(&msg.nonce), sizeof(msg.nonce));
  return msg.encode();
}

std::vector<ByteBuffer> Multiplexer::handle_control(const ByteBuffer& payload) {
  std::vector<ByteBuffer> replies;
  auto msg = ControlMessage::decode(payload.data(), payload.size());
  if (!msg) return replies;

  switch (msg->kind) {
    case ControlKind::Ping: {
      ControlMessage pong;
      pong.kind = ControlKind::Pong;
      pong.nonce = msg->nonce;
      replies.push_back(pong.encode());
      break;
    }
    case ControlKind::Pong:
      break;
    case ControlKind::OpenStream:
      streams_[msg->stream_id] = Stream{msg->stream_type, {}, true};
      break;
    case ControlKind::CloseStream:
      if (streams_.count(msg->stream_id)) streams_[msg->stream_id].open = false;
      break;
  }
  return replies;
}

void Multiplexer::push(uint32_t stream_id, ByteBuffer data) {
  streams_[stream_id].queue.push_back(std::move(data));
}

}  // namespace nyx
