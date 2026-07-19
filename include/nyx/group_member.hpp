#pragma once

/** @file group_member.hpp
 *  Клиент поля: подключение к hub, GroupJoin, групповой чат (фаза 5).
 */

#include "nyx/connection.hpp"
#include "nyx/group.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"

#include "nyx/outbox.hpp"

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace nyx {

struct GroupRecordView {
  GroupId id{};
  std::string name;
  std::string description;
  std::string direction;
  std::string tags;
  GroupVisibility visibility = GroupVisibility::Circle;
  /** true после GroupMeta от hub — можно перезаписывать локальную мету. */
  bool meta_received = false;
  std::vector<GroupMemberRecord> members;
};

/** Сессия участника поля (не owner hub). */
class GroupMemberService {
 public:
  using MessageCallback = std::function<void(const ChatMessage&, bool outgoing)>;
  using DeliveryCallback =
      std::function<void(uint64_t message_id, DeliveryStatus status)>;
  using EventCallback = std::function<void(const std::string& text)>;
  using MetaCallback = std::function<void()>;

  GroupMemberService(Connection& connection, Profile profile, GroupId group_id,
                     std::string group_name);

  /** После Hello: отправляет GroupJoin и ждёт JoinAck. */
  bool join(int timeout_ms = 10000);

  /** Отправка в поле; false если hub мёртв / не joined. */
  bool send_message(const std::string& text, uint64_t* out_id = nullptr);
  void handle_payload(const ByteBuffer& payload);
  /** Keep-alive; при таймауте peer сбрасывает joined. */
  void tick();

  void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
  void set_on_delivery(DeliveryCallback cb) { on_delivery_ = std::move(cb); }
  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }
  void set_on_meta(MetaCallback cb) { on_meta_ = std::move(cb); }

  const GroupRecordView& view() const { return view_; }
  bool joined() const { return joined_; }
  const ChatId& chat_id() const { return chat_id_; }

 private:
  ChatMessage make_message(const std::string& text) const;
  StoredMessage to_stored(const ChatMessage& msg, bool outgoing) const;
  void deliver_incoming(ChatMessage msg);

  Connection& connection_;
  Profile profile_;
  GroupId group_id_{};
  std::string group_name_;
  ChatId chat_id_{};
  MessageStore store_;
  GroupRecordView view_;
  bool joined_ = false;

  void apply_meta(const GroupMetaMessage& meta);

  MessageCallback on_message_;
  DeliveryCallback on_delivery_;
  EventCallback on_event_;
  MetaCallback on_meta_;
  std::unordered_set<uint64_t> pending_acks_;
};

}  // namespace nyx
