#pragma once

/** @file group_hub.hpp
 *  Hub поля (star): создатель принимает несколько Connection на одном UDP-сокете.
 */

#include "nyx/connection.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"
#include "nyx/udp.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nyx {

struct UserIdHash {
  std::size_t operator()(const UserId& id) const {
    std::size_t h = 0;
    for (uint8_t b : id) h = h * 31 + b;
    return h;
  }
};

/** Сессия участника на hub. */
struct HubMember {
  Connection connection;
  UserId user_id{};
  std::string nickname;
  bool joined = false;
};

/** Центральный узел поля: relay MsgV2 всем участникам. */
class GroupHub {
 public:
  using MessageCallback = std::function<void(const ChatMessage&, bool outgoing)>;
  using EventCallback = std::function<void(const std::string& text)>;

  GroupHub(UdpSocket socket, Profile owner, GroupRecord group);

  /** Один цикл: recv с сокета, drive участников, accept новых handshake. */
  void poll();

  /** Отправка сообщения от owner в групповой чат. */
  bool send_message(const std::string& text);

  void handle_chat_payload(HubMember& member, const ByteBuffer& payload);

  void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }

  /** Индекс, scope и ACL для kBulkStream на соединениях участников. */
  void attach_files(FileIndex& index, const GroupId& share_scope,
                    FileAccessStore* access = nullptr);

  const GroupRecord& group() const { return group_; }
  GroupRecord& group() { return group_; }
  const std::vector<HubMember>& members() const { return members_; }
  UdpSocket& socket() { return socket_; }
  MessageStore& store() { return store_; }

  /** Отключает участника и обновляет roster. */
  bool remove_member(const UserId& user_id);

  /** Bye всем участникам перед остановкой hub (мгновенный офлайн у клиентов). */
  void notify_shutdown(const std::string& reason = "hub остановлен");

  /** Рассылает актуальную ACL всем участникам поля. */
  void broadcast_file_access_policy();

  /** Каталог share-корней поля с учётом ACL (без рекурсивного дампа файлов). */
  std::vector<FileEntry> catalog_for(const UserId& requester) const;
  /** Один уровень внутри share-корня (подпапки-маркеры + файлы). */
  std::vector<FileEntry> catalog_level_for(const UserId& requester, const std::string& root_path,
                                          const std::string& parent_rel) const;

  /** Копирует файл из локального индекса hub в dest_path; проверяет hash. */
  bool download_local_file(const FileHash& hash, const std::string& dest_path,
                           std::string* saved_path = nullptr) const;

 private:
  void send_file_access_policy(HubMember& member);
  HubMember* find_member(const std::string& host, uint16_t port);
  bool try_accept(const std::string& host, uint16_t port, const ByteBuffer& first_packet);
  void complete_join(HubMember& member);
  void send_history_to(HubMember& member);
  void relay_message(const ChatMessage& msg, const UserId* exclude_author);
  void broadcast_to_members(const ByteBuffer& payload, HubMember* skip);
  /** Удаляет участников с мёртвым keep-alive (roster в group_ не трогает). */
  void drop_stale_members();
  StoredMessage to_stored(const ChatMessage& msg, bool outgoing) const;
  ChatMessage make_owner_message(const std::string& text) const;
  FileTransferService& file_service_for(HubMember& member);

  void handle_member_bulk(HubMember& member, const ByteBuffer& payload);
  std::vector<FileEntry> merged_field_entries() const;
  std::vector<FileEntry> merged_field_entries_for(const UserId& requester) const;
  HubMember* find_hash_provider(const FileHash& hash);
  void rebuild_hash_providers();
  void relay_file_request(HubMember& provider, HubMember& requester, const FileHash& hash);

  UdpSocket socket_;
  Profile owner_;
  GroupRecord group_;
  ChatId chat_id_{};
  MessageStore store_;
  std::vector<HubMember> members_;

  MessageCallback on_message_;
  EventCallback on_event_;

  FileIndex* file_index_ = nullptr;
  FileAccessStore* file_access_ = nullptr;
  GroupId file_scope_{};
  std::unordered_map<HubMember*, std::unique_ptr<FileTransferService>> file_services_;
  std::unordered_map<UserId, std::vector<FileEntry>, UserIdHash> member_catalog_;
  std::unordered_map<UserId, std::vector<std::string>, UserIdHash> member_roots_;
  std::unordered_map<std::string, UserId, std::hash<std::string>> hash_providers_;

  struct FileRelay {
    HubMember* requester = nullptr;
    HubMember* provider = nullptr;
    FileHash hash{};
  };
  std::optional<FileRelay> active_relay_;
};

}  // namespace nyx
