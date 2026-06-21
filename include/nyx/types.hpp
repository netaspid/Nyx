#pragma once

/** @file types.hpp
 *  Базовые типы и константы протокола Nyx v1.
 */

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

constexpr uint32_t kMagic = 0x3158594Eu; // "NYX1" (LE on wire)
constexpr uint8_t kProtocolVersion = 1;
constexpr uint32_t kControlStream = 0;
constexpr uint32_t kChatStream = 1;
constexpr uint32_t kBulkStream = 2;
constexpr std::size_t kHeaderSize = 18;
constexpr std::size_t kMaxPayload = 65535;
constexpr std::size_t kDefaultMtu = 1200;
constexpr std::size_t kInviteTokenSize = 32;

using InviteToken = std::array<uint8_t, kInviteTokenSize>;

/** Тип UDP-кадра на проводе. */
enum class PacketType : uint8_t {
  HandshakeInit = 0x01,
  HandshakeResp = 0x02,
  HandshakeFinish = 0x03,
  Data = 0x10,
  Ack = 0x11,
  Nack = 0x12,
  KeepAlive = 0x20,
  RendezvousRegister = 0x30,
  RendezvousLookup = 0x31,
  RendezvousResponse = 0x32,
};

/** Роль узла в Noise handshake. */
enum class HandshakeRole { Initiator, Responder };

/** Назначение логического потока в мультиплексоре. */
enum class StreamType : uint8_t {
  Control = 0,
  Data = 1,
  Bulk = 2,
  Realtime = 3,
};

/** Жизненный цикл Connection. */
enum class ConnectionState { Handshaking, Established, Closed };

using ByteBuffer = std::vector<uint8_t>;

}  // namespace nyx
