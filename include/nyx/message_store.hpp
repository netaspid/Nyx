#pragma once

/** @file message_store.hpp
 *  Локальная история сообщений (JSON-lines на диске).
 */

#include "nyx/chat_id.hpp"
#include "nyx/messaging.hpp"

#include <string>
#include <vector>

namespace nyx {

/** Запись в истории чата. */
struct StoredMessage {
  uint64_t id = 0;
  uint64_t timestamp_ms = 0;
  std::string chat_id_hex;
  std::string author;
  std::string author_id_hex;
  std::string text;
  bool outgoing = false;
};

/** Хранилище переписки (файл .jsonl). */
class MessageStore {
 public:
  explicit MessageStore(std::string path);

  /** Переключает файл истории (после JoinAck, когда GroupId стал известен).
   *  @param path новый путь .jsonl */
  void rebind(std::string path);

  /** Добавляет сообщение и дописывает строку в файл. */
  bool append(const StoredMessage& message);

  /** true если сообщение с таким id уже в истории (дедуп после JoinAck). */
  bool contains_id(uint64_t id) const;

  /** Последние count сообщений из памяти/файла. */
  std::vector<StoredMessage> recent(std::size_t count) const;

  /** Поиск по подстроке в text/author (без учёта регистра). */
  std::vector<StoredMessage> search(const std::string& query, std::size_t limit) const;

  /** Путь к файлу истории по ChatId. */
  static std::string path_for_chat(const ChatId& chat_id);

  /** История группового поля: data_dir()/groups/<group_id_hex>.jsonl. */
  static std::string path_for_group(const GroupId& group_id);

  /** @deprecated используйте path_for_chat(dm_chat_id(...)). */
  static std::string chat_path(const UserId& peer_id);

 private:
  bool load_from_disk() const;

  std::string path_;
  mutable std::vector<StoredMessage> cache_;
  mutable bool loaded_ = false;
};

}  // namespace nyx
