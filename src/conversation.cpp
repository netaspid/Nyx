#include "nyx/conversation.hpp"

#include "nyx/app.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/group.hpp"
#include "nyx/message_store.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <filesystem>

namespace nyx {

namespace {

void push_if_newer(std::vector<ConversationSummary>& out, ConversationSummary item) {
  for (const auto& existing : out) {
    if (existing.key == item.key) {
      if (item.timestamp_ms > existing.timestamp_ms) {
        for (auto& e : out) {
          if (e.key == item.key) {
            e = std::move(item);
            break;
          }
        }
      }
      return;
    }
  }
  out.push_back(std::move(item));
}

std::optional<StoredMessage> last_message(const std::string& store_path) {
  MessageStore store(store_path);
  const auto recent = store.recent(1);
  if (recent.empty()) return std::nullopt;
  return recent.back();
}

}  // namespace

std::string format_last_seen(uint64_t last_seen_ms, uint64_t now_ms) {
  if (last_seen_ms == 0) return "не в сети";
  if (now_ms <= last_seen_ms) return "в сети";
  const uint64_t delta_sec = (now_ms - last_seen_ms) / 1000;
  if (delta_sec < 120) return "был(а) только что";
  if (delta_sec < 3600) return "был(а) " + std::to_string(delta_sec / 60) + " мин назад";
  if (delta_sec < 86400) return "был(а) " + std::to_string(delta_sec / 3600) + " ч назад";
  return "был(а) " + std::to_string(delta_sec / 86400) + " дн назад";
}

std::vector<ConversationSummary> list_conversations(const UserId& self) {
  std::vector<ConversationSummary> out;

  ContactBook book(default_contacts_path());
  book.load();
  for (const auto& contact : book.contacts()) {
    const ChatId cid = dm_chat_id(self, contact.user_id);
    const std::string path = MessageStore::path_for_chat(cid);
    ConversationSummary item;
    item.key = "dm:" + to_hex(contact.user_id.data(), contact.user_id.size());
    item.title = contact.nickname.empty() ? short_user_id(contact.user_id) : contact.nickname;
    item.kind = ConversationKind::Direct;
    item.peer_id_hex = to_hex(contact.user_id.data(), contact.user_id.size());
    item.last_seen_ms = contact.last_seen_ms;
    if (auto last = last_message(path)) {
      item.preview = last->text;
      item.timestamp_ms = last->timestamp_ms;
    }
    push_if_newer(out, std::move(item));
  }

  GroupStore groups;
  groups.load();
  for (const auto& group : groups.all()) {
    const std::string path = MessageStore::path_for_group(group.id);
    ConversationSummary item;
    item.key = "group:" + GroupStore::group_id_hex(group.id);
    item.title = group.name;
    item.kind = ConversationKind::Group;
    item.group_id_hex = GroupStore::group_id_hex(group.id);
    if (auto last = last_message(path)) {
      item.preview = last->author.empty() ? last->text : (last->author + ": " + last->text);
      item.timestamp_ms = last->timestamp_ms;
    }
    push_if_newer(out, std::move(item));
  }

  std::error_code ec;
  const std::string chats_dir = data_dir() + "/chats";
  if (std::filesystem::is_directory(chats_dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(chats_dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      if (entry.path().extension() != ".jsonl") continue;
      const std::string stem = entry.path().stem().string();
      const bool already =
          std::any_of(out.begin(), out.end(), [&](const ConversationSummary& s) {
            return s.key.find(stem) != std::string::npos ||
                   s.peer_id_hex == stem || s.group_id_hex == stem;
          });
      if (already) continue;
      if (auto last = last_message(entry.path().string())) {
        ConversationSummary item;
        item.key = "chat:" + stem;
        item.title = stem.substr(0, 8) + "…";
        item.preview = last->text;
        item.timestamp_ms = last->timestamp_ms;
        item.kind = ConversationKind::Direct;
        push_if_newer(out, std::move(item));
      }
    }
  }

  std::sort(out.begin(), out.end(), [](const ConversationSummary& a, const ConversationSummary& b) {
    if (a.timestamp_ms != b.timestamp_ms) return a.timestamp_ms > b.timestamp_ms;
    return a.title < b.title;
  });
  return out;
}

}  // namespace nyx
