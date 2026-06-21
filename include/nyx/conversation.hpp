#pragma once

/** @file conversation.hpp
 *  Сводка переписок для списка чатов GUI (контакты + поля + история на диске).
 */

#include "nyx/identity.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

/** Тип переписки в списке чатов. */
enum class ConversationKind : uint8_t {
  Direct = 0,
  Group = 1,
};

/** Элемент списка чатов (локальные данные). */
struct ConversationSummary {
  std::string key;
  std::string title;
  std::string preview;
  uint64_t timestamp_ms = 0;
  ConversationKind kind = ConversationKind::Direct;
  std::string peer_id_hex;
  std::string group_id_hex;
  uint64_t last_seen_ms = 0;
};

/** Собирает DM + группы с последним сообщением из MessageStore. */
std::vector<ConversationSummary> list_conversations(const UserId& self);

/** Человекочитаемый last seen («был(а) N мин назад»). */
std::string format_last_seen(uint64_t last_seen_ms, uint64_t now_ms);

}  // namespace nyx
