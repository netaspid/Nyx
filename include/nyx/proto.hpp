#pragma once

#include "nyx/types.hpp"

#include <optional>
#include <string>

namespace nyx {

struct FrameHeader {
  uint8_t version = kProtocolVersion;
  PacketType packet_type = PacketType::Data;
  uint16_t flags = 0;
  uint32_t stream_id = 0;
  uint32_t seq_num = 0;
  uint16_t payload_length = 0;
};

struct Frame {
  FrameHeader header;
  ByteBuffer payload;

  static Frame make(PacketType type, uint32_t stream_id, uint32_t seq, ByteBuffer payload);

  ByteBuffer encode() const;

  static std::optional<Frame>
  decode(const uint8_t* data, std::size_t len, std::string* err = nullptr);
};

struct EndpointHint {
  std::array<uint8_t, 16> ip {};
  uint16_t port = 0;
  std::array<uint8_t, 8> nonce {};
  ByteBuffer encode() const;
  static std::optional<EndpointHint> decode(const uint8_t* data, std::size_t len);

  std::string host_string() const;
};

enum class RendezvousKind : uint8_t {
  Register = 0x01,
  Lookup = 0x02,
  Response = 0x03,
  NotFound = 0x04,
  Unregister = 0x05,
};

struct RendezvousMessage {
  RendezvousKind kind = RendezvousKind::NotFound;
  InviteToken token {};
  EndpointHint hint {};

  ByteBuffer encode() const;
  static std::optional<RendezvousMessage> decode(const uint8_t* data, std::size_t len);
};

enum class ControlKind : uint8_t {
  Ping = 0x01,
  Pong = 0x02,
  OpenStream = 0x03,
  CloseStream = 0x04,
  Rekey = 0x05,
};

struct ControlMessage {
  ControlKind kind = ControlKind::Ping;
  uint64_t nonce = 0;
  uint32_t stream_id = 0;
  StreamType stream_type = StreamType::Control;

  ByteBuffer encode() const;
  static std::optional<ControlMessage> decode(const uint8_t* data, std::size_t len);
};

bool is_handshake_datagram(const ByteBuffer& data);

bool is_punch_datagram(const ByteBuffer& data);

} // namespace nyx
