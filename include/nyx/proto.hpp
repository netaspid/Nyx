#pragma once

/** @file proto.hpp
 *  Кодирование кадров Nyx и служебных сообщений (rendezvous, control).
 */

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

/** Кадр протокола: заголовок + payload (+ опционально CRC). */
struct Frame {
  FrameHeader header;
  ByteBuffer payload;
  std::optional<uint32_t> crc;

  /** Собирает кадр с заполненным payload_length. */
  static Frame make(PacketType type, uint32_t stream_id, uint32_t seq,
                    ByteBuffer payload);

  /** Сериализация в байты для UDP. Пустой буфер если payload слишком большой. */
  ByteBuffer encode() const;

  /** Разбор буфера. @param err необязательное описание ошибки. */
  static std::optional<Frame> decode(const uint8_t* data, std::size_t len,
                                     std::string* err = nullptr);
};

/** Подсказка адреса для rendezvous (26 байт: ip, port, nonce). */
struct EndpointHint {
  std::array<uint8_t, 16> ip{};
  uint16_t port = 0;
  std::array<uint8_t, 8> nonce{};
  ByteBuffer encode() const;
  static std::optional<EndpointHint> decode(const uint8_t* data, std::size_t len);

  /** IPv4-строка из поля ip. */
  std::string host_string() const;
};

enum class RendezvousKind : uint8_t {
  Register = 0x01,
  Lookup = 0x02,
  Response = 0x03,
  NotFound = 0x04,
};

/** Сообщение bootstrap-сервера: регистрация или поиск по invite token. */
struct RendezvousMessage {
  RendezvousKind kind = RendezvousKind::NotFound;
  InviteToken token{};
  EndpointHint hint{};

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

/** Служебное сообщение на потоке 0 (Ping/Pong, open/close stream). */
struct ControlMessage {
  ControlKind kind = ControlKind::Ping;
  uint64_t nonce = 0;
  uint32_t stream_id = 0;
  StreamType stream_type = StreamType::Control;

  ByteBuffer encode() const;
  static std::optional<ControlMessage> decode(const uint8_t* data, std::size_t len);
};

/** Datagram содержит кадр handshake (Init/Resp/Finish). */
bool is_handshake_datagram(const ByteBuffer& data);

/** Datagram hole-punch probe (NYX-PUNCH). */
bool is_punch_datagram(const ByteBuffer& data);

/** IPv4-строка из поля ip подсказки rendezvous. */
std::string endpoint_hint_host(const EndpointHint& hint);

}  // namespace nyx
