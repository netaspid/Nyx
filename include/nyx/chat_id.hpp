#pragma once

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

using ChatId = std::array<uint8_t, 32>;
using GroupId = std::array<uint8_t, 32>;

ChatId dm_chat_id(const UserId& self, const UserId& peer);

ChatId group_chat_id(const GroupId& group_id);

std::string chat_id_hex(const ChatId& id);

}
