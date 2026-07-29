#pragma once

/** @file messaging.hpp
 *  Messenger message formats: ChatMessage, Bye, Ack.
 */

#include "nyx/chat_id.hpp"
#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Frame type on kChatStream. */
enum class ChatKind : uint8_t {
  Hello = 1,
  Text = 2, // legacy: raw UTF-8 without metadata
  Msg = 3,  // ChatMessage without chat_id (legacy)
  Bye = 4,
  Ack = 5,
  MsgV2 = 6, // ChatMessage with chat_id
};

/** Chat text message. */
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

/** Peer disconnect notification. */
struct ByeMessage {
  std::string reason;

  ByteBuffer encode() const;
  static std::optional<ByeMessage> decode(const ByteBuffer& data);
};

/** ChatMessage delivery acknowledgement. */
struct AckMessage {
  uint64_t message_id = 0;

  ByteBuffer encode() const;
  static std::optional<AckMessage> decode(const ByteBuffer& data);
};

/** Next message id (monotonic). */
uint64_t next_message_id();

/** Current UTC time in milliseconds. */
uint64_t now_ms();

} // namespace nyx
