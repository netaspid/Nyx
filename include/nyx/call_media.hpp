#pragma once

/** @file call_media.hpp
 *  Кадры медиа на kRealtimeStream (без ARQ).
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <optional>

namespace nyx {

enum class CallMediaType : uint8_t {
  Opus = 1,
  Video = 2,
  Fec = 3,
  Nack = 4,
};

struct CallMediaFrame {
  CallMediaType type = CallMediaType::Opus;
  uint32_t seq = 0;
  ByteBuffer payload;

  ByteBuffer encode() const;
  static std::optional<CallMediaFrame> decode(const ByteBuffer& data);
};

/** Рекомендуемый потолок payload под MTU realtime. */
constexpr std::size_t kMaxCallMediaPayload = 1000;

}  // namespace nyx
