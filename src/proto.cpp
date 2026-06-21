#include "nyx/proto.hpp"

#include "nyx/util.hpp"

#include <cstdio>
#include <cstring>

namespace nyx {

Frame Frame::make(PacketType type, uint32_t stream_id, uint32_t seq,
                  ByteBuffer payload) {
  Frame f;
  f.header.packet_type = type;
  f.header.stream_id = stream_id;
  f.header.seq_num = seq;
  f.payload = std::move(payload);
  f.header.payload_length = static_cast<uint16_t>(f.payload.size());
  return f;
}

ByteBuffer Frame::encode() const {
  if (payload.size() > kMaxPayload) return {};
  ByteBuffer out;
  out.reserve(kHeaderSize + payload.size());
  write_u32_le(out, kMagic);
  out.push_back(kProtocolVersion);
  out.push_back(static_cast<uint8_t>(header.packet_type));
  write_u16_le(out, header.flags);
  write_u32_le(out, header.stream_id);
  write_u32_le(out, header.seq_num);
  write_u16_le(out, static_cast<uint16_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::optional<Frame> Frame::decode(const uint8_t* data, std::size_t len,
                                     std::string* err) {
  if (len < kHeaderSize) {
    if (err) *err = "buffer too short";
    return std::nullopt;
  }
  if (read_u32_le(data) != kMagic) {
    if (err) *err = "invalid magic";
    return std::nullopt;
  }
  if (data[4] != kProtocolVersion) {
    if (err) *err = "unsupported version";
    return std::nullopt;
  }
  Frame f;
  f.header.version = data[4];
  f.header.packet_type = static_cast<PacketType>(data[5]);
  f.header.flags = read_u16_le(data + 6);
  f.header.stream_id = read_u32_le(data + 8);
  f.header.seq_num = read_u32_le(data + 12);
  f.header.payload_length = read_u16_le(data + 16);
  std::size_t total = kHeaderSize + f.header.payload_length;
  if (len < total) {
    if (err) *err = "payload truncated";
    return std::nullopt;
  }
  f.payload.assign(data + kHeaderSize, data + total);
  return f;
}

ByteBuffer EndpointHint::encode() const {
  ByteBuffer out;
  out.reserve(26);
  out.insert(out.end(), ip.begin(), ip.end());
  write_u16_le(out, port);
  out.insert(out.end(), nonce.begin(), nonce.end());
  return out;
}

std::optional<EndpointHint> EndpointHint::decode(const uint8_t* data,
                                                  std::size_t len) {
  if (len < 26) return std::nullopt;
  EndpointHint h;
  std::copy(data, data + 16, h.ip.begin());
  h.port = read_u16_le(data + 16);
  std::copy(data + 18, data + 26, h.nonce.begin());
  return h;
}

ByteBuffer RendezvousMessage::encode() const {
  ByteBuffer out;
  switch (kind) {
    case RendezvousKind::Register:
      out.push_back(0x01);
      out.insert(out.end(), token.begin(), token.end());
      {
        auto eh = hint.encode();
        out.insert(out.end(), eh.begin(), eh.end());
      }
      break;
    case RendezvousKind::Lookup:
      out.push_back(0x02);
      out.insert(out.end(), token.begin(), token.end());
      break;
    case RendezvousKind::Response:
      out.push_back(0x03);
      {
        auto eh = hint.encode();
        out.insert(out.end(), eh.begin(), eh.end());
      }
      break;
    case RendezvousKind::NotFound:
      out.push_back(0x04);
      break;
  }
  return out;
}

std::optional<RendezvousMessage> RendezvousMessage::decode(const uint8_t* data,
                                                            std::size_t len) {
  if (len < 1) return std::nullopt;
  RendezvousMessage m;
  switch (data[0]) {
    case 0x01:
      if (len < 1 + 32 + 26) return std::nullopt;
      std::copy(data + 1, data + 33, m.token.begin());
      if (auto hint = EndpointHint::decode(data + 33, len - 33)) {
        m.hint = *hint;
      } else {
        return std::nullopt;
      }
      m.kind = RendezvousKind::Register;
      break;
    case 0x02:
      if (len < 33) return std::nullopt;
      std::copy(data + 1, data + 33, m.token.begin());
      m.kind = RendezvousKind::Lookup;
      break;
    case 0x03:
      if (len < 27) return std::nullopt;
      if (auto hint = EndpointHint::decode(data + 1, len - 1)) {
        m.hint = *hint;
      } else {
        return std::nullopt;
      }
      m.kind = RendezvousKind::Response;
      break;
    case 0x04:
      m.kind = RendezvousKind::NotFound;
      break;
    default:
      return std::nullopt;
  }
  return m;
}

ByteBuffer ControlMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(kind));
  switch (kind) {
    case ControlKind::Ping:
    case ControlKind::Pong:
    case ControlKind::Rekey:
      write_u64_le(out, nonce);
      break;
    case ControlKind::OpenStream:
      write_u32_le(out, stream_id);
      out.push_back(static_cast<uint8_t>(stream_type));
      break;
    case ControlKind::CloseStream:
      write_u32_le(out, stream_id);
      break;
  }
  return out;
}

std::optional<ControlMessage> ControlMessage::decode(const uint8_t* data,
                                                      std::size_t len) {
  if (len < 1) return std::nullopt;
  ControlMessage m;
  m.kind = static_cast<ControlKind>(data[0]);
  switch (m.kind) {
    case ControlKind::Ping:
    case ControlKind::Pong:
    case ControlKind::Rekey:
      if (len < 9) return std::nullopt;
      m.nonce = read_u64_le(data + 1);
      break;
    case ControlKind::OpenStream:
      if (len < 6) return std::nullopt;
      m.stream_id = read_u32_le(data + 1);
      m.stream_type = static_cast<StreamType>(data[5]);
      break;
    case ControlKind::CloseStream:
      if (len < 5) return std::nullopt;
      m.stream_id = read_u32_le(data + 1);
      break;
  }
  return m;
}

bool is_handshake_datagram(const ByteBuffer& data) {
  auto frame = Frame::decode(data.data(), data.size());
  if (!frame) return false;
  const auto type = frame->header.packet_type;
  return type == PacketType::HandshakeInit || type == PacketType::HandshakeResp ||
         type == PacketType::HandshakeFinish;
}

bool is_punch_datagram(const ByteBuffer& data) {
  return data.size() >= 10 && std::memcmp(data.data(), "NYX-PUNCH", 10) == 0;
}

std::string EndpointHint::host_string() const {
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[12], ip[13], ip[14], ip[15]);
  return buf;
}

std::string endpoint_hint_host(const EndpointHint& hint) { return hint.host_string(); }

}  // namespace nyx
