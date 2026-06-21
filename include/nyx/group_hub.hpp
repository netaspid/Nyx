#pragma once

/** @file group_hub.hpp
 *  Hub поля (star): создатель принимает несколько Connection на одном UDP-сокете.
 */

#include "nyx/connection.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
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

  /** Индекс и scope для kBulkStream на соединениях участников. */
  void attach_files(FileIndex& index, const GroupId& share_scope);

  const GroupRecord& group() const { return group_; }
  const std::vector<HubMember>& members() const { return members_; }
  UdpSocket& socket() { return socket_; }
  MessageStore& store() { return store_; }

 private:
  HubMember* find_member(const std::string& host, uint16_t port);
  bool try_accept(const std::string& host, uint16_t port, const ByteBuffer& first_packet);
  void complete_join(HubMember& member);
  void relay_message(const ChatMessage& msg, const UserId* exclude_author);
  void broadcast_to_members(const ByteBuffer& payload, HubMember* skip);
  StoredMessage to_stored(const ChatMessage& msg, bool outgoing) const;
  ChatMessage make_owner_message(const std::string& text) const;
  FileTransferService& file_service_for(HubMember& member);

  UdpSocket socket_;
  Profile owner_;
  GroupRecord group_;
  ChatId chat_id_{};
  MessageStore store_;
  std::vector<HubMember> members_;

  MessageCallback on_message_;
  EventCallback on_event_;

  FileIndex* file_index_ = nullptr;
  GroupId file_scope_{};
  std::unordered_map<HubMember*, std::unique_ptr<FileTransferService>> file_services_;
};

}  // namespace nyx
