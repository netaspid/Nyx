#pragma once

/** @file chat_id.hpp
 *  Conversation id: direct message (DM) or field (group).
 */

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

using ChatId = std::array<uint8_t, 32>;
using GroupId = std::array<uint8_t, 32>;

/** Stable DM id for a user pair (SHA-256 of sorted pubkeys). */
ChatId dm_chat_id(const UserId& self, const UserId& peer);

/** Group chat id: SHA-256("group" || group_id). */
ChatId group_chat_id(const GroupId& group_id);

/** Hex form used in history file paths. */
std::string chat_id_hex(const ChatId& id);

}  // namespace nyx
