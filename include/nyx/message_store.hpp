#pragma once

/** @file message_store.hpp
 *  Local message history (JSON lines on disk).
 */

#include "nyx/chat_id.hpp"
#include "nyx/messaging.hpp"

#include <string>
#include <vector>

namespace nyx {

/** Chat history record. */
struct StoredMessage {
  uint64_t id = 0;
  uint64_t timestamp_ms = 0;
  std::string chat_id_hex;
  std::string author;
  std::string author_id_hex;
  std::string text;
  bool outgoing = false;
};

/** Conversation storage (.jsonl file). */
class MessageStore {
 public:
  explicit MessageStore(std::string path);

  /** Switches the history file (after JoinAck, once the GroupId is known).
   *  @param path new .jsonl path */
  void rebind(std::string path);

  /** Appends a message and writes the line to the file. */
  bool append(const StoredMessage& message);

  /** true when a message with this id is already stored (dedup after JoinAck). */
  bool contains_id(uint64_t id) const;

  /** Last count messages from memory/file. */
  std::vector<StoredMessage> recent(std::size_t count) const;

  /** Case-insensitive substring search in text/author. */
  std::vector<StoredMessage> search(const std::string& query, std::size_t limit) const;

  /** History file path for a ChatId. */
  static std::string path_for_chat(const ChatId& chat_id);

  /** Field group history: data_dir()/groups/<group_id_hex>.jsonl. */
  static std::string path_for_group(const GroupId& group_id);

  /** @deprecated use path_for_chat(dm_chat_id(...)). */
  static std::string chat_path(const UserId& peer_id);

 private:
  bool load_from_disk() const;

  std::string path_;
  mutable std::vector<StoredMessage> cache_;
  mutable bool loaded_ = false;
};

}  // namespace nyx
