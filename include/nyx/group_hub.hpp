#pragma once

#include "nyx/call_proto.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"
#include "nyx/outbox.hpp"
#include "nyx/udp.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nyx {

struct UserIdHash {
  std::size_t operator()(const UserId& id) const {
    std::size_t h = 0;
    for (uint8_t b : id)
      h = h * 31 + b;
    return h;
  }
};

struct HubMember {
  Connection connection;
  UserId user_id {};
  std::string nickname;
  bool joined = false;
};

class GroupHub {
public:
  using MessageCallback = std::function<void(const ChatMessage&, bool outgoing)>;
  using DeliveryCallback = std::function<void(uint64_t message_id, DeliveryStatus status)>;
  using EventCallback = std::function<void(const std::string& text)>;
  using CallFrameCallback = std::function<void(const UserId& from, const ByteBuffer& frame)>;

  GroupHub(UdpSocket socket, Profile owner, GroupRecord group);


  void poll();

  bool send_message(const std::string& text);

  bool send_call_frame(const ByteBuffer& frame, const UserId* skip_user = nullptr);
  void distribute_call_mesh_intros(const CallId& call_id, const std::vector<UserId>& participants);

  bool send_realtime_all(const ByteBuffer& data);

  void relay_realtime(const std::function<void(const UserId& from, ByteBuffer)>& on_local);

  void handle_chat_payload(HubMember& member, const ByteBuffer& payload);

  void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
  void set_on_delivery(DeliveryCallback cb) { on_delivery_ = std::move(cb); }
  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }
  void set_on_call_frame(CallFrameCallback cb) { on_call_frame_ = std::move(cb); }


  void
  attach_files(FileIndex& index, const GroupId& share_scope, FileAccessStore* access = nullptr);


  bool set_member_role(const UserId& user_id, GroupRole role);


  GroupRole role_of(const UserId& user_id) const;

  const GroupRecord& group() const { return group_; }
  GroupRecord& group() { return group_; }
  const std::vector<HubMember>& members() const { return members_; }
  UdpSocket& socket() { return socket_; }
  MessageStore& store() { return store_; }


  bool remove_member(const UserId& user_id);


  void notify_shutdown(const std::string& reason = "эфир закрыт");


  void broadcast_file_access_policy();


  bool publish_meta(const std::string& description,
                    const std::string& direction,
                    const std::string& tags,
                    GroupVisibility visibility);

  void send_meta_to(HubMember& member);
  void broadcast_meta();


  std::vector<FileEntry> catalog_for(const UserId& requester) const;

  std::vector<FileEntry> catalog_level_for(const UserId& requester,
                                           const std::string& root_path,
                                           const std::string& parent_rel) const;


  bool download_local_file(const FileHash& hash,
                           const std::string& dest_path,
                           std::string* saved_path = nullptr) const;


  bool request_file_from_provider(const FileHash& hash, const std::string& dest_path);


  bool provider_transfer_busy(const FileHash& hash) const;

  void set_on_file_complete(FileTransferService::CompletionCallback cb) {
    on_file_complete_ = std::move(cb);
  }
  void set_on_file_progress(FileTransferService::ProgressCallback cb) {
    on_file_progress_ = std::move(cb);
  }

private:
  void send_file_access_policy(HubMember& member);
  HubMember* find_member(const std::string& host, uint16_t port);
  bool try_accept(const std::string& host, uint16_t port, const ByteBuffer& first_packet);
  void complete_join(HubMember& member);
  void send_history_to(HubMember& member);
  void broadcast_to_members(const ByteBuffer& payload, HubMember* skip);

  void drop_stale_members();
  StoredMessage to_stored(const ChatMessage& msg, bool outgoing) const;
  ChatMessage make_owner_message(const std::string& text) const;
  FileTransferService& file_service_for(HubMember& member);

  void handle_member_bulk(HubMember& member, const ByteBuffer& payload);
  std::vector<FileEntry> merged_field_entries() const;
  std::vector<FileEntry> merged_field_entries_for(const UserId& requester) const;
  HubMember* find_hash_provider(const FileHash& hash);
  void rebuild_hash_providers();
  void relay_file_request(HubMember& provider,
                          HubMember& requester,
                          const FileHash& hash,
                          const ByteBuffer& request);

  UdpSocket socket_;
  Profile owner_;
  GroupRecord group_;
  ChatId chat_id_ {};
  MessageStore store_;
  std::vector<HubMember> members_;

  MessageCallback on_message_;
  DeliveryCallback on_delivery_;
  EventCallback on_event_;
  CallFrameCallback on_call_frame_;
  FileTransferService::CompletionCallback on_file_complete_;
  FileTransferService::ProgressCallback on_file_progress_;

  std::unordered_set<uint64_t> pending_member_acks_;

  FileIndex* file_index_ = nullptr;
  FileAccessStore* file_access_ = nullptr;
  GroupId file_scope_ {};
  std::unordered_map<HubMember*, std::unique_ptr<FileTransferService>> file_services_;
  std::unordered_map<UserId, std::vector<FileEntry>, UserIdHash> member_catalog_;
  std::unordered_map<UserId, std::vector<std::string>, UserIdHash> member_roots_;
  std::unordered_map<UserId, uint64_t, UserIdHash> member_catalog_revisions_;
  std::unordered_map<std::string, UserId, std::hash<std::string>> hash_providers_;

  struct FileRelay {
    HubMember* requester = nullptr;
    HubMember* provider = nullptr;
    FileHash hash {};
  };
  std::optional<FileRelay> active_relay_;
};

}
