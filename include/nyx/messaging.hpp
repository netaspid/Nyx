#pragma once

#include "nyx/chat_id.hpp"
#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

enum class ChatKind : uint8_t {
  Hello = 1,
  Text = 2,
  Msg = 3,
  Bye = 4,
  Ack = 5,
  MsgV2 = 6,
};

struct ChatMessage {
  uint64_t id = 0;
  uint64_t timestamp_ms = 0;
  ChatId chat_id {};
  UserId author_id {};
  std::string author;
  std::string text;

  ByteBuffer encode() const;
  static std::optional<ChatMessage> decode(const ByteBuffer& data);
};

struct ByeMessage {
  std::string reason;

  ByteBuffer encode() const;
  static std::optional<ByeMessage> decode(const ByteBuffer& data);
};

struct AckMessage {
  uint64_t message_id = 0;

  ByteBuffer encode() const;
  static std::optional<AckMessage> decode(const ByteBuffer& data);
};

uint64_t next_message_id();

uint64_t now_ms();

} // namespace nyx
