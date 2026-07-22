#include "nyx/group_hub.hpp"

#include "nyx/app.hpp"
#include "nyx/avatar_proto.hpp"
#include "nyx/avatar_store.hpp"
#include "nyx/call_proto.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/group_proto.hpp"
#include "nyx/messaging.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/proto.hpp"
#include "nyx/file_proto.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace nyx {

GroupHub::GroupHub(UdpSocket socket, Profile owner, GroupRecord group)
    : socket_(std::move(socket)),
      owner_(std::move(owner)),
      group_(std::move(group)),
      chat_id_(group_chat_id(group_.id)),
      store_(MessageStore::path_for_group(group_.id)) {}

void GroupHub::attach_files(FileIndex& index, const GroupId& share_scope,
                            FileAccessStore* access) {
  file_index_ = &index;
  file_access_ = access;
  file_scope_ = share_scope;
  file_services_.clear();
  member_catalog_.clear();
  member_roots_.clear();
  hash_providers_.clear();
  active_relay_.reset();
}

void GroupHub::rebuild_hash_providers() {
  hash_providers_.clear();
  for (const auto& [uid, entries] : member_catalog_) {
    (void)uid;
    for (const auto& e : entries) {
      hash_providers_[hash_hex(e.hash)] = uid;
    }
  }
}

std::vector<FileEntry> GroupHub::merged_field_entries() const {
  std::vector<FileEntry> out;
  std::set<std::string> seen_paths;
  if (file_index_) {
    out = file_index_->listing_for_session(file_scope_);
    for (const auto& e : out) {
      if (e.is_directory()) seen_paths.insert(e.root_path);
    }
  }
  std::set<std::string> seen_hashes;
  for (const auto& e : out) seen_hashes.insert(hash_hex(e.hash));

  for (const auto& [uid, entries] : member_catalog_) {
    (void)uid;
    for (const auto& e : entries) {
      if (e.is_directory()) continue;
      const std::string hx = hash_hex(e.hash);
      if (seen_hashes.count(hx)) continue;
      seen_hashes.insert(hx);
      out.push_back(e);
    }
  }

  for (const auto& [uid, roots] : member_roots_) {
    (void)uid;
    for (const auto& path : roots) {
      if (seen_paths.count(path)) continue;
      seen_paths.insert(path);
      ShareRoot sr;
      sr.path = path;
      sr.group_id = file_scope_;
      int count = 0;
      const auto cat = member_catalog_.find(uid);
      if (cat != member_catalog_.end()) {
        for (const auto& e : cat->second) {
          if (e.root_path == path) ++count;
        }
      }
      out.insert(out.begin(), FileIndex::make_directory_marker(sr, count, "участник: "));
    }
  }
  return out;
}

std::vector<FileEntry> GroupHub::merged_field_entries_for(const UserId& requester) const {
  const auto out = merged_field_entries();
  if (!file_access_) return out;
  if (requester == owner_.public_key) return out;

  std::vector<FileEntry> filtered;
  filtered.reserve(out.size());
  for (const auto& e : out) {
    // Маркер корня: ACL по share-root (relative_path у маркера — только подпись для UI).
    const std::string rel_for_acl = e.is_directory() ? std::string{} : e.relative_path;
    const uint32_t perms =
        file_access_->permissions_for(file_scope_, requester, e.root_path, rel_for_acl);
    if (!FileAccessStore::has_permission(perms, FilePermission::List)) continue;
    filtered.push_back(e);
  }
  return filtered;
}

std::vector<FileEntry> GroupHub::catalog_for(const UserId& requester) const {
  std::vector<FileEntry> roots;
  for (const auto& e : merged_field_entries_for(requester)) {
    if (e.is_directory()) roots.push_back(e);
  }
  return roots;
}

std::vector<FileEntry> GroupHub::catalog_level_for(const UserId& requester,
                                                   const std::string& root_path,
                                                   const std::string& parent_rel) const {
  return FileIndex::listing_level(merged_field_entries_for(requester), root_path, parent_rel);
}

bool GroupHub::download_local_file(const FileHash& hash, const std::string& dest_path,
                                   std::string* saved_path) const {
  if (!file_index_ || dest_path.empty()) return false;
  const auto entry = file_index_->find_for_session(hash, file_scope_);
  if (!entry) return false;

  std::error_code ec;
  const auto fs_dest = path_from_utf8(dest_path);
  const auto parent = fs_dest.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }

  std::filesystem::copy_file(path_from_utf8(entry->absolute_path()), fs_dest,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return false;

  FileHash verify{};
  if (!hash_file(dest_path, verify) || verify != hash) {
    std::filesystem::remove(fs_dest, ec);
    return false;
  }
  if (saved_path) *saved_path = dest_path;
  return true;
}

HubMember* GroupHub::find_hash_provider(const FileHash& hash) {
  const auto it = hash_providers_.find(hash_hex(hash));
  if (it == hash_providers_.end()) return nullptr;
  for (auto& m : members_) {
    if (m.joined && m.user_id == it->second) return &m;
  }
  return nullptr;
}

void GroupHub::relay_file_request(HubMember& provider, HubMember& requester,
                                  const FileHash& hash) {
  active_relay_ = FileRelay{&requester, &provider, hash};
  FileRequest req;
  req.hash = hash;
  provider.connection.send_payload(kBulkStream, req.encode());
}

void GroupHub::handle_member_bulk(HubMember& member, const ByteBuffer& payload) {
  if (payload.empty()) return;

  if (is_avatar_frame(payload)) {
    if (auto req = AvatarRequest::decode(payload)) {
      AvatarStore store;
      store.load();
      ByteBuffer data;
      if (!store.read_bytes(req->hash, data)) {
        AvatarDeny deny;
        deny.hash = req->hash;
        deny.reason = "нет фото";
        member.connection.send_payload(kBulkStream, deny.encode());
        return;
      }
      std::string mime = "image/jpeg";
      for (const auto& e : store.photos()) {
        if (e.hash == req->hash) {
          mime = e.mime;
          break;
        }
      }
      AvatarOffer offer;
      offer.hash = req->hash;
      offer.size = data.size();
      offer.mime = mime;
      member.connection.send_payload(kBulkStream, offer.encode());
      uint32_t index = 0;
      for (std::size_t off = 0; off < data.size(); off += kAvatarChunkSize) {
        AvatarChunk chunk;
        chunk.hash = req->hash;
        chunk.index = index++;
        const std::size_t n = std::min(kAvatarChunkSize, data.size() - off);
        chunk.data.assign(data.begin() + static_cast<std::ptrdiff_t>(off),
                         data.begin() + static_cast<std::ptrdiff_t>(off + n));
        member.connection.send_payload(kBulkStream, chunk.encode());
      }
      AvatarDone done;
      done.hash = req->hash;
      member.connection.send_payload(kBulkStream, done.encode());
      return;
    }
    return;
  }

  const auto kind = static_cast<FileKind>(payload[0]);

  if (kind == FileKind::ListReq) {
    std::vector<FileEntry> entries;
    if (auto path = decode_list_request(payload)) {
      if (path->first.empty()) {
        entries = catalog_for(member.user_id);
      } else {
        entries = catalog_level_for(member.user_id, path->first, path->second);
      }
    } else {
      entries = catalog_for(member.user_id);
    }
    member.connection.send_payload(kBulkStream, encode_list_response(entries));
    return;
  }

  if (kind == FileKind::PolicyReq) {
    send_file_access_policy(member);
    return;
  }

  if (kind == FileKind::IndexPush) {
    if (auto push = decode_index_push(payload)) {
      member_catalog_[member.user_id] = push->entries;
      member_roots_[member.user_id] = push->root_paths;
      rebuild_hash_providers();
      if (on_event_) {
        on_event_(member.nickname + " опубликовал " + std::to_string(push->entries.size()) +
                  " файлов, " + std::to_string(push->root_paths.size()) + " папок");
      }
    }
    return;
  }

  if (active_relay_ && active_relay_->provider == &member) {
    if (kind == FileKind::Offer || kind == FileKind::Chunk || kind == FileKind::Complete ||
        kind == FileKind::Deny) {
      if (active_relay_->requester) {
        active_relay_->requester->connection.send_payload(kBulkStream, payload);
      }
      if (kind == FileKind::Complete || kind == FileKind::Deny) active_relay_.reset();
      return;
    }
  }

  if (kind == FileKind::Request) {
    if (auto req = FileRequest::decode(payload)) {
      std::optional<FileEntry> entry;
      if (file_index_) entry = file_index_->find_by_hash(req->hash);
      if (!entry) {
        for (const auto& [uid, catalog] : member_catalog_) {
          (void)uid;
          for (const auto& e : catalog) {
            if (e.hash == req->hash) {
              entry = e;
              break;
            }
          }
          if (entry) break;
        }
      }
      if (entry && file_access_ && member.user_id != owner_.public_key) {
        const uint32_t perms = file_access_->permissions_for(file_scope_, member.user_id,
                                                             entry->root_path,
                                                             entry->relative_path);
        if (!FileAccessStore::has_permission(perms, FilePermission::Download)) {
          FileDeny deny;
          deny.hash = req->hash;
          deny.reason = "нет права скачивания";
          member.connection.send_payload(kBulkStream, deny.encode());
          return;
        }
      }
      if (file_index_ && file_index_->find_for_session(req->hash, file_scope_)) {
        file_service_for(member).handle_bulk(payload);
        return;
      }
      if (HubMember* provider = find_hash_provider(req->hash)) {
        if (provider != &member) {
          relay_file_request(*provider, member, req->hash);
          return;
        }
      }
    }
  }

  file_service_for(member).handle_bulk(payload);
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

  // push_back может переаллоцировать vector — указатели HubMember* в map становятся висячими.
  file_services_.clear();
  active_relay_.reset();
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
    if (m.connection.state() != ConnectionState::Established) continue;
    m.connection.send_payload(kChatStream, payload);
  }
}

bool GroupHub::send_call_frame(const ByteBuffer& frame, const UserId* skip_user) {
  if (!is_call_frame(frame)) return false;
  for (auto& m : members_) {
    if (!m.joined) continue;
    if (skip_user && m.user_id == *skip_user) continue;
    if (m.connection.state() != ConnectionState::Established) continue;
    m.connection.send_payload(kChatStream, frame);
  }
  return true;
}

void GroupHub::distribute_call_mesh_intros(const CallId& call_id) {
  CallRosterMessage roster;
  roster.call_id = call_id;
  roster.participants.push_back(owner_.public_key);
  for (const auto& m : members_) {
    if (!m.joined) continue;
    roster.participants.push_back(m.user_id);
    if (roster.participants.size() >= kMaxCallParticipants) break;
  }
  send_call_frame(roster.encode());

  auto send_intro = [&](HubMember& target, const UserId& uid, const std::string& host,
                        uint16_t port) {
    CallPeerIntroMessage intro;
    intro.call_id = call_id;
    intro.peer.user_id = uid;
    intro.peer.host = host;
    intro.peer.port = port;
    target.connection.send_payload(kChatStream, intro.encode());
  };

  const std::string owner_host = guess_lan_ipv4();
  for (auto& target : members_) {
    if (!target.joined) continue;
    // Owner → member (порт уточнит Endpoint от owner).
    send_intro(target, owner_.public_key, owner_host, socket_.local_port());
    for (const auto& src : members_) {
      if (!src.joined || src.user_id == target.user_id) continue;
      send_intro(target, src.user_id, src.connection.peer_host(), src.connection.peer_port());
    }
  }
}

bool GroupHub::send_realtime_all(const ByteBuffer& data) {
  bool any = false;
  for (auto& m : members_) {
    if (!m.joined) continue;
    if (m.connection.send_realtime(data)) any = true;
  }
  return any;
}

void GroupHub::drain_realtime(const std::function<void(ByteBuffer)>& on_frame) {
  if (!on_frame) return;
  for (auto& m : members_) {
    if (!m.joined) continue;
    ByteBuffer raw;
    while (m.connection.recv_realtime(raw)) on_frame(std::move(raw));
  }
}

void GroupHub::relay_realtime(const std::function<void(ByteBuffer)>& on_local) {
  for (auto& m : members_) {
    if (!m.joined) continue;
    ByteBuffer raw;
    while (m.connection.recv_realtime(raw)) {
      if (on_local) on_local(raw);
      for (auto& o : members_) {
        if (!o.joined || o.user_id == m.user_id) continue;
        if (o.connection.state() != ConnectionState::Established) continue;
        o.connection.send_realtime(raw);
      }
    }
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
    if (m.connection.state() != ConnectionState::Established) continue;
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
  send_meta_to(member);

  send_history_to(member);

  GroupMemberJoinedMessage notice;
  notice.member = rec;
  broadcast_to_members(notice.encode(), &member);

  if (on_event_) {
    on_event_(member.nickname + " вошёл в поле «" + group_.name + "»");
  }

  send_file_access_policy(member);
}

void GroupHub::send_meta_to(HubMember& member) {
  GroupMetaMessage meta;
  meta.description = group_.description;
  meta.direction = group_.direction;
  meta.tags = group_.tags;
  meta.visibility = group_.visibility;
  member.connection.send_payload(kChatStream, meta.encode());
}

void GroupHub::broadcast_meta() {
  GroupMetaMessage meta;
  meta.description = group_.description;
  meta.direction = group_.direction;
  meta.tags = group_.tags;
  meta.visibility = group_.visibility;
  broadcast_to_members(meta.encode(), nullptr);
}

bool GroupHub::publish_meta(const std::string& description, const std::string& direction,
                            const std::string& tags, GroupVisibility visibility) {
  group_.description = description;
  group_.direction = direction;
  group_.tags = tags;
  group_.visibility = visibility;
  {
    GroupStore store;
    store.load();
    store.upsert(group_);
    store.save();
  }
  broadcast_meta();
  return true;
}

GroupRole GroupHub::role_of(const UserId& user_id) const {
  if (user_id == owner_.public_key) return GroupRole::Owner;
  for (const auto& m : group_.members) {
    if (m.user_id == user_id) return m.role;
  }
  return GroupRole::Member;
}

bool GroupHub::set_member_role(const UserId& user_id, GroupRole role) {
  if (user_id == owner_.public_key) return false;
  if (role == GroupRole::Owner) return false;
  if (role != GroupRole::Member && role != GroupRole::Host) return false;

  GroupMemberRecord* rec = nullptr;
  for (auto& m : group_.members) {
    if (m.user_id == user_id) {
      rec = &m;
      break;
    }
  }
  if (!rec) return false;
  rec->role = role;

  {
    GroupStore store;
    store.load();
    store.upsert(group_);
    store.save();
  }

  GroupMemberJoinedMessage notice;
  notice.member = *rec;
  broadcast_to_members(notice.encode(), nullptr);
  if (on_event_) {
    on_event_(rec->nickname + (role == GroupRole::Host ? " — ведущий звонков"
                                                       : " — обычный участник"));
  }
  return true;
}

void GroupHub::send_file_access_policy(HubMember& member) {
  if (!file_access_) return;
  const GroupFileAccess* policy = file_access_->find_policy(file_scope_);
  if (!policy) return;
  member.connection.send_payload(kBulkStream, encode_policy_push(*policy));
}

void GroupHub::broadcast_file_access_policy() {
  if (!file_access_) return;
  const GroupFileAccess* policy = file_access_->find_policy(file_scope_);
  if (!policy) return;
  const ByteBuffer wire = encode_policy_push(*policy);
  for (auto& m : members_) {
    if (!m.joined) continue;
    m.connection.send_payload(kBulkStream, wire);
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
      send_meta_to(member);
      send_history_to(member);
      return;
    }
    complete_join(member);
    return;
  }

  if (GroupMemberJoinedMessage::decode(payload)) return;
  if (GroupMetaMessage::decode(payload)) return;
  if (GroupJoinAckMessage::decode(payload)) return;

  if (auto ack = AckMessage::decode(payload)) {
    if (pending_member_acks_.erase(ack->message_id) > 0 && on_delivery_) {
      on_delivery_(ack->message_id, DeliveryStatus::Delivered);
    }
    return;
  }

  if (is_call_frame(payload)) {
    if (auto inv = CallInviteMessage::decode(payload)) {
      GroupRole role = GroupRole::Member;
      for (const auto& rec : group_.members) {
        if (rec.user_id == member.user_id) {
          role = rec.role;
          break;
        }
      }
      if (member.user_id == owner_.public_key) role = GroupRole::Owner;
      if (!can_start_field_call(role)) {
        CallRejectMessage rej;
        rej.call_id = inv->call_id;
        rej.reason = CallRejectReason::Unsupported;
        member.connection.send_payload(kChatStream, rej.encode());
        if (on_event_) on_event_("отклонён старт звонка: нет роли ведущего");
        return;
      }
    }
    if (on_call_frame_) on_call_frame_(member.user_id, payload);
    for (auto& m : members_) {
      if (!m.joined) continue;
      if (m.user_id == member.user_id) continue;
      if (m.connection.state() != ConnectionState::Established) continue;
      m.connection.send_payload(kChatStream, payload);
    }
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
        if (m.connection.state() != ConnectionState::Established) continue;
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

  int live_joined = 0;
  for (const auto& m : members_) {
    if (m.joined && m.connection.state() == ConnectionState::Established) ++live_joined;
  }
  broadcast_to_members(wire, nullptr);
  if (live_joined == 0) {
    // Никого в эфире — сообщение в локальной истории (уедет с history при join).
    if (on_delivery_) on_delivery_(msg.id, DeliveryStatus::Delivered);
  } else {
    pending_member_acks_.insert(msg.id);
  }
  return true;
}

void GroupHub::poll() {
  drop_stale_members();

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
        } else if (stream_id == kBulkStream) {
          if (is_avatar_frame(payload) || file_index_) {
            handle_member_bulk(*member, payload);
          }
        }
      }
    }
  }
}

void GroupHub::drop_stale_members() {
  bool removed = false;
  for (auto it = members_.begin(); it != members_.end();) {
    if (it->connection.state() == ConnectionState::Established &&
        it->connection.drive_without_recv()) {
      if (file_index_ && it->joined) {
        file_service_for(*it).pump();
      }
      ++it;
      continue;
    }

    if (it->connection.state() == ConnectionState::Handshaking) {
      ++it;
      continue;
    }

    const std::string nick = it->nickname.empty() ? std::string("участник") : it->nickname;
    const UserId uid = it->user_id;
    if (it->joined && on_event_) {
      on_event_(nick + " отключился от поля");
    }
    it = members_.erase(it);
    member_catalog_.erase(uid);
    member_roots_.erase(uid);
    removed = true;
  }
  if (removed) {
    // Указатели HubMember* в map после erase невалидны.
    file_services_.clear();
    rebuild_hash_providers();
  }
}

void GroupHub::notify_shutdown(const std::string& reason) {
  ByeMessage bye;
  bye.reason = reason;
  const ByteBuffer wire = bye.encode();
  for (auto& m : members_) {
    if (!m.joined) continue;
    if (m.connection.state() != ConnectionState::Established) continue;
    m.connection.send_payload(kChatStream, wire);
    m.connection.drive_without_recv();
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
      member_catalog_.erase(user_id);
      member_roots_.erase(user_id);
      file_services_.clear();
      rebuild_hash_providers();
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
