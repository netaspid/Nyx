#include "nyx/group_hub.hpp"

#include "nyx/app.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/paths.hpp"
#include "nyx/proto.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>

namespace nyx {

GroupHub::GroupHub(UdpSocket socket, Profile owner, GroupRecord group)
    : socket_(std::move(socket)),
      owner_(std::move(owner)),
      group_(std::move(group)),
      chat_id_(group_chat_id(group_.id)),
      store_(MessageStore::path_for_group(group_.id)) {}

void GroupHub::attach_files(FileIndex& index, const GroupId& share_scope) {
  file_index_ = &index;
  file_scope_ = share_scope;
  file_services_.clear();
}

FileTransferService& GroupHub::file_service_for(HubMember& member) {
  auto it = file_services_.find(&member);
  if (it != file_services_.end()) return *it->second;

  auto fs = std::make_unique<FileTransferService>(member.connection, *file_index_,
                                                  default_downloads_dir());
  fs->set_share_scope(file_scope_);
  if (on_event_) fs->set_on_event(on_event_);
  it = file_services_.emplace(&member, std::move(fs)).first;
  return *it->second;
}

HubMember* GroupHub::find_member(const std::string& host, uint16_t port) {
  for (auto& m : members_) {
    if (m.connection.peer_host() == host && m.connection.peer_port() == port) {
      return &m;
    }
  }
  return nullptr;
}

bool GroupHub::try_accept(const std::string& host, uint16_t port,
                          const ByteBuffer& first_packet) {
  if (find_member(host, port)) return false;

  auto conn = Connection::accept_responder(socket_, host, port, &first_packet);
  if (!conn) return false;

  HubMember member{std::move(*conn), {}, "", false};
  members_.push_back(std::move(member));
  if (on_event_) on_event_("входящее соединение " + host + ':' + std::to_string(port));
  return true;
}

StoredMessage GroupHub::to_stored(const ChatMessage& msg, bool outgoing) const {
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

ChatMessage GroupHub::make_owner_message(const std::string& text) const {
  ChatMessage msg;
  msg.id = next_message_id();
  msg.timestamp_ms = now_ms();
  msg.chat_id = chat_id_;
  msg.author_id = owner_.public_key;
  msg.author = owner_.nickname;
  msg.text = text;
  return msg;
}

void GroupHub::broadcast_to_members(const ByteBuffer& payload, HubMember* skip) {
  for (auto& m : members_) {
    if (!m.joined) continue;
    if (skip && &m == skip) continue;
    m.connection.send_payload(kChatStream, payload);
  }
}

void GroupHub::relay_message(const ChatMessage& msg, const UserId* exclude_author) {
  (void)exclude_author;
  store_.append(to_stored(msg, msg.author_id == owner_.public_key));
  if (on_message_) {
    on_message_(msg, msg.author_id == owner_.public_key);
  }
  const ByteBuffer wire = msg.encode();
  for (auto& m : members_) {
    if (!m.joined) continue;
    if (m.user_id == msg.author_id) continue;
    m.connection.send_payload(kChatStream, wire);
  }
}

void GroupHub::send_history_to(HubMember& member) {
  const auto history = store_.recent(100);
  for (const auto& stored : history) {
    ChatMessage msg;
    msg.id = stored.id;
    msg.timestamp_ms = stored.timestamp_ms;
    msg.chat_id = chat_id_;
    msg.author = stored.author;
    msg.text = stored.text;
    std::vector<uint8_t> author_bytes;
    if (from_hex(stored.author_id_hex, author_bytes) &&
        author_bytes.size() == msg.author_id.size()) {
      std::memcpy(msg.author_id.data(), author_bytes.data(), author_bytes.size());
    }
    member.connection.send_payload(kChatStream, msg.encode());
  }
}

void GroupHub::complete_join(HubMember& member) {
  member.joined = true;

  GroupMemberRecord rec;
  rec.user_id = member.user_id;
  rec.nickname = member.nickname;
  rec.role = GroupRole::Member;

  bool found = false;
  for (auto& m : group_.members) {
    if (m.user_id == rec.user_id) {
      m.nickname = rec.nickname;
      found = true;
      break;
    }
  }
  if (!found) group_.members.push_back(rec);

  {
    GroupStore store;
    store.load();
    store.upsert(group_);
    store.save();
  }

  GroupJoinAckMessage ack;
  ack.accepted = true;
  ack.group_id = group_.id;
  ack.group_name = group_.name;
  ack.members = group_.members;
  member.connection.send_payload(kChatStream, ack.encode());

  send_history_to(member);

  GroupMemberJoinedMessage notice;
  notice.member = rec;
  broadcast_to_members(notice.encode(), &member);

  if (on_event_) {
    on_event_(member.nickname + " вошёл в поле «" + group_.name + "»");
  }
}

void GroupHub::handle_chat_payload(HubMember& member, const ByteBuffer& payload) {
  if (auto hello = decode_hello_message(payload)) {
    member.user_id = hello->public_key;
    member.nickname = hello->nickname;

    HelloMessage reply;
    reply.public_key = owner_.public_key;
    reply.nickname = owner_.nickname;
    member.connection.send_payload(kChatStream, reply.encode());

    if (on_event_) {
      on_event_(hello->nickname + " в сети (id: " + short_user_id(hello->public_key) + ")");
    }
    return;
  }

  if (auto join = GroupJoinMessage::decode(payload)) {
    const bool id_ok = join->group_id == group_.id;
    const bool id_empty = std::all_of(join->group_id.begin(), join->group_id.end(),
                                      [](uint8_t b) { return b == 0; });
    if (!id_ok && !id_empty) {
      GroupJoinAckMessage deny;
      deny.accepted = false;
      deny.reason = "неверный group_id";
      member.connection.send_payload(kChatStream, deny.encode());
      return;
    }
    if (member.user_id == owner_.public_key) {
      member.joined = true;
      GroupJoinAckMessage ack;
      ack.accepted = true;
      ack.group_id = group_.id;
      ack.group_name = group_.name;
      ack.members = group_.members;
      member.connection.send_payload(kChatStream, ack.encode());
      send_history_to(member);
      return;
    }
    complete_join(member);
    return;
  }

  if (GroupMemberJoinedMessage::decode(payload)) return;
  if (GroupJoinAckMessage::decode(payload)) return;

  if (auto ack = AckMessage::decode(payload)) {
    (void)ack;
    return;
  }

  if (auto msg = ChatMessage::decode(payload)) {
    if (msg->chat_id != chat_id_) return;
    if (!member.joined && msg->author_id != owner_.public_key) return;

    const bool from_owner = msg->author_id == owner_.public_key;
    if (!from_owner && msg->author_id != member.user_id) return;

    store_.append(to_stored(*msg, from_owner));
    if (on_message_) on_message_(*msg, from_owner);

    const ByteBuffer wire = msg->encode();
    if (from_owner) {
      broadcast_to_members(wire, nullptr);
    } else {
      for (auto& m : members_) {
        if (!m.joined) continue;
        if (m.user_id == member.user_id) continue;
        m.connection.send_payload(kChatStream, wire);
      }
      if (on_event_) on_event_("relay «" + msg->text + "» от " + msg->author);
    }

    AckMessage ack;
    ack.message_id = msg->id;
    member.connection.send_payload(kChatStream, ack.encode());
    return;
  }
}

bool GroupHub::send_message(const std::string& text) {
  ChatMessage msg = make_owner_message(text);
  store_.append(to_stored(msg, true));
  if (on_message_) on_message_(msg, true);
  const ByteBuffer wire = msg.encode();
  broadcast_to_members(wire, nullptr);
  return true;
}

void GroupHub::poll() {
  for (auto& m : members_) {
    m.connection.drive_without_recv();
    if (file_index_ && m.joined) {
      file_service_for(m).pump();
    }
  }

  std::string host;
  uint16_t port = 0;
  while (auto pkt = socket_.recv_from(host, port, 0)) {
    if (is_punch_datagram(*pkt)) continue;

    if (is_handshake_datagram(*pkt)) {
      if (!find_member(host, port)) {
        try_accept(host, port, *pkt);
      }
      continue;
    }

    if (auto* member = find_member(host, port)) {
      member->connection.feed_wire(*pkt);
      ByteBuffer payload;
      uint32_t stream_id = 0;
      while (member->connection.pop_stream(stream_id, payload)) {
        if (stream_id == kChatStream) {
          handle_chat_payload(*member, payload);
        } else if (stream_id == kBulkStream && file_index_) {
          file_service_for(*member).handle_bulk(payload);
        }
      }
    }
  }
}

bool GroupHub::remove_member(const UserId& user_id) {
  if (user_id == owner_.public_key) return false;

  group_.members.erase(
      std::remove_if(group_.members.begin(), group_.members.end(),
                     [&](const GroupMemberRecord& m) { return m.user_id == user_id; }),
      group_.members.end());

  for (auto it = members_.begin(); it != members_.end();) {
    if (it->user_id == user_id) {
      if (on_event_) {
        on_event_(it->nickname + " исключён из поля");
      }
      it = members_.erase(it);
    } else {
      ++it;
    }
  }

  GroupStore store;
  store.load();
  store.remove_member(group_.id, user_id);
  if (auto updated = store.find(group_.id)) {
    group_ = *updated;
  }
  return true;
}

}  // namespace nyx
