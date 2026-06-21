#pragma once

/** @file messaging.hpp
 *  Формат сообщений мессенджера (фаза 3): ChatMessage, Bye, Ack.
 */

#include "nyx/chat_id.hpp"
#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Тип кадра на kChatStream. */
enum class ChatKind : uint8_t {
  Hello = 1,
  Text = 2,   // legacy: сырой UTF-8 без метаданных
  Msg = 3,    // ChatMessage без chat_id (legacy)
  Bye = 4,
  Ack = 5,
  MsgV2 = 6,  // ChatMessage с chat_id (фаза 3+)
};

/** Текстовое сообщение чата. */
struct ChatMessage {
  uint64_t id = 0;
  uint64_t timestamp_ms = 0;
  ChatId chat_id{};
  UserId author_id{};
  std::string author;
  std::string text;

  ByteBuffer encode() const;
  static std::optional<ChatMessage> decode(const ByteBuffer& data);
};

/** Уведомление об отключении peer. */
struct ByeMessage {
  std::string reason;

  ByteBuffer encode() const;
  static std::optional<ByeMessage> decode(const ByteBuffer& data);
};

/** Подтверждение доставки ChatMessage. */
struct AckMessage {
  uint64_t message_id = 0;

  ByteBuffer encode() const;
  static std::optional<AckMessage> decode(const ByteBuffer& data);
};

/** Следующий id сообщения (монотонный, thread-safe enough для CLI). */
uint64_t next_message_id();

/** Текущее время в миллисекундах UTC. */
uint64_t now_ms();

}  // namespace nyx
