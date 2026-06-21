#include "nyx/group_member.hpp"

#include "nyx/app.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <thread>

namespace nyx {

GroupMemberService::GroupMemberService(Connection& connection, Profile profile,
                                       GroupId group_id, std::string group_name)
    : connection_(connection),
      profile_(std::move(profile)),
      group_id_(group_id),
      group_name_(std::move(group_name)),
      chat_id_(group_chat_id(group_id_)),
      store_(MessageStore::path_for_group(group_id_)) {
  view_.id = group_id_;
  view_.name = group_name_;
}

ChatMessage GroupMemberService::make_message(const std::string& text) const {
  ChatMessage msg;
  msg.id = next_message_id();
  msg.timestamp_ms = now_ms();
  msg.chat_id = chat_id_;
  msg.author_id = profile_.public_key;
  msg.author = profile_.nickname;
  msg.text = text;
  return msg;
}

StoredMessage GroupMemberService::to_stored(const ChatMessage& msg, bool outgoing) const {
  StoredMessage stored;
  stored.id = msg.id;
  stored.timestamp_ms = msg.timestamp_ms;
  stored.chat_id_hex = chat_id_hex(msg.chat_id);
  stored.author = msg.author;
  stored.author_id_hex = to_hex(msg.author_id.data(), msg.author_id.size());
  stored.text = msg.text;
  stored.outgoing = outgoing;
  return stored;
}

bool GroupMemberService::join(int timeout_ms) {
  GroupJoinMessage join;
  join.group_id = group_id_;
  if (!connection_.send_payload(kChatStream, join.encode())) return false;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    connection_.drive();
    ByteBuffer payload;
    uint32_t stream_id = 0;
    while (connection_.recv_stream(stream_id, payload)) {
      if (stream_id != kChatStream) continue;
      if (auto ack = GroupJoinAckMessage::decode(payload)) {
        if (!ack->accepted) {
          if (on_event_) on_event_("отказ: " + ack->reason);
          return false;
        }
        joined_ = true;
        group_id_ = ack->group_id;
        view_.id = ack->group_id;
        view_.name = ack->group_name;
        view_.members = std::move(ack->members);
        chat_id_ = group_chat_id(group_id_);
        group_name_ = view_.name;
        if (on_event_) {
          on_event_("в поле «" + view_.name + "» (" +
                    std::to_string(view_.members.size()) + " участников)");
        }

        const auto history_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < history_deadline) {
          connection_.drive();
          ByteBuffer payload;
          uint32_t stream_id = 0;
          while (connection_.recv_stream(stream_id, payload)) {
            if (stream_id != kChatStream) continue;
            handle_payload(payload);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
      }
      handle_payload(payload);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (on_event_) on_event_("таймаут GroupJoin");
  return false;
}

void GroupMemberService::deliver_incoming(ChatMessage msg) {
  store_.append(to_stored(msg, false));
  if (on_message_) on_message_(msg, false);

  AckMessage ack;
  ack.message_id = msg.id;
  connection_.send_payload(kChatStream, ack.encode());
}

bool GroupMemberService::send_message(const std::string& text, uint64_t* out_id) {
  if (!joined_) return false;
  ChatMessage msg = make_message(text);
  if (!connection_.send_payload(kChatStream, msg.encode())) return false;
  store_.append(to_stored(msg, true));
  if (on_message_) on_message_(msg, true);
  if (out_id) *out_id = msg.id;
  return true;
}

void GroupMemberService::handle_payload(const ByteBuffer& payload) {
  if (is_group_frame(payload)) {
    if (auto notice = GroupMemberJoinedMessage::decode(payload)) {
      view_.members.push_back(notice->member);
      if (on_event_) {
        on_event_(notice->member.nickname + " присоединился к полю");
      }
      return;
    }
    if (GroupJoinAckMessage::decode(payload)) return;
    if (GroupJoinMessage::decode(payload)) return;
  }

  if (decode_hello_message(payload)) return;

  if (auto ack = AckMessage::decode(payload)) {
    (void)ack;
    return;
  }

  if (auto msg = ChatMessage::decode(payload)) {
    if (msg->chat_id != chat_id_) return;
    deliver_incoming(std::move(*msg));
  }
}

void GroupMemberService::tick() { connection_.drive(); }

}  // namespace nyx
