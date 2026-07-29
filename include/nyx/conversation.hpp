#pragma once

/** @file conversation.hpp
 *  Conversation summaries for the GUI chat list (contacts + fields + on-disk history).
 */

#include "nyx/identity.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

enum class ConversationKind : uint8_t {
  Direct = 0,
  Group = 1,
};

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

std::vector<ConversationSummary> list_conversations(const UserId& self);

std::string format_last_seen(uint64_t last_seen_ms, uint64_t now_ms);

} // namespace nyx
