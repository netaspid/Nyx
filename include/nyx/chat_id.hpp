#pragma once

/** @file chat_id.hpp
 *  Идентификатор переписки: личка (DM) или поле (группа, фаза 5).
 */

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

using ChatId = std::array<uint8_t, 32>;
using GroupId = std::array<uint8_t, 32>;

/** Стабильный id лички для пары пользователей (SHA-256 от sorted pubkeys). */
ChatId dm_chat_id(const UserId& self, const UserId& peer);

/** Id группового чата (фаза 5): SHA-256("group" || group_id). */
ChatId group_chat_id(const GroupId& group_id);

/** Hex-представление для путей к файлам истории. */
std::string chat_id_hex(const ChatId& id);

}  // namespace nyx
