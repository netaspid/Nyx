#pragma once

#include "nyx/chat_id.hpp"
#include "nyx/messaging.hpp"

#include <string>
#include <vector>

namespace nyx {

struct StoredMessage {
  uint64_t id = 0;
  uint64_t timestamp_ms = 0;
  std::string chat_id_hex;
  std::string author;
  std::string author_id_hex;
  std::string text;
  bool outgoing = false;
};

class MessageStore {
public:
  explicit MessageStore(std::string path);

  void rebind(std::string path);

  bool append(const StoredMessage& message);

  bool contains_id(uint64_t id) const;

  std::vector<StoredMessage> recent(std::size_t count) const;

  std::vector<StoredMessage> search(const std::string& query, std::size_t limit) const;

  static std::string path_for_chat(const ChatId& chat_id);

  static std::string path_for_group(const GroupId& group_id);

  static std::string chat_path(const UserId& peer_id);

private:
  bool load_from_disk() const;

  std::string path_;
  mutable std::vector<StoredMessage> cache_;
  mutable bool loaded_ = false;
};

} // namespace nyx
