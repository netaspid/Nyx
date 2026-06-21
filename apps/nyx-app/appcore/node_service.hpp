#pragma once

/** @file node_service.hpp
 *  AppCore без Qt: listen/connect/chat/files/groups для nyx-app и тестов.
 */

#include "connection_label.hpp"
#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/identity.hpp"
#include "nyx/mdns.hpp"
#include "nyx/messaging.hpp"
#include "nyx/network_config.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nyx_app {

struct UiMessage {
  uint64_t timestamp_ms = 0;
  std::string author;
  std::string text;
  bool outgoing = false;
};

enum class NodeMode {
  Idle,
  Listening,
  ChatDirect,
  GroupHub,
  GroupMember,
};

/** Оркестратор сетевых сценариев (фоновый поток, callbacks). */
class NodeService {
 public:
  using StatusCallback = std::function<void(const std::string&)>;
  using MessageCallback = std::function<void(const UiMessage&)>;
  using TokenCallback = std::function<void(const std::string& token_hex)>;
  using LanPeersCallback = std::function<void(const std::vector<nyx::LanPeer>&)>;
  using GroupInfoCallback = std::function<void(const std::string& group_id_hex,
                                               const std::string& invite_hex)>;
  using ChatReadyCallback =
      std::function<void(const std::string& peer_title, const std::string& connection_label)>;

  NodeService();
  ~NodeService();

  NodeService(const NodeService&) = delete;
  NodeService& operator=(const NodeService&) = delete;

  void set_profile_path(std::string path);
  void set_nickname(std::string nickname);
  void set_rendezvous(std::string addr);
  void set_rendezvous_list(const std::string& csv);
  void set_discovery_mode(int mode);
  bool save_network_config();
  bool load_network_config();
  bool test_rendezvous(const std::string& host, uint16_t port);
  const nyx::NetworkConfig& network_config() const { return network_config_; }
  std::string rendezvous_list_string() const;

  void set_on_status(StatusCallback cb);
  void set_on_message(MessageCallback cb);
  void set_on_invite_token(TokenCallback cb);
  void set_on_lan_peers(LanPeersCallback cb);
  void set_on_group_created(GroupInfoCallback cb);
  void set_on_chat_ready(ChatReadyCallback cb);

  using FileProgressCallback =
      std::function<void(const std::string& label, int percent)>;
  void set_on_file_progress(FileProgressCallback cb);

  NodeMode mode() const { return mode_.load(); }
  bool busy() const { return busy_.load(); }

  nyx::Profile profile() const;

  bool start_listen(bool lan_advertise = true);
  bool start_connect_token(const std::string& token_hex);
  bool start_connect_peer(const std::string& host, uint16_t port);
  bool start_browse(int timeout_ms = 3000);
  bool scan_lan_peers(int timeout_ms = 2000);

  bool create_group(const std::string& name);
  bool start_group_hub(const std::string& group_id_hex);
  bool start_group_join(const std::string& invite_hex);

  void stop();

  bool send_message(const std::string& text);
  bool send_bye(const std::string& reason);
  bool index_folder(const std::string& path);
  bool request_remote_files();
  bool download_file(const std::string& hash_hex);
  bool send_file(const std::string& path_or_hash);

  std::vector<nyx::StoredMessage> chat_history(std::size_t count = 50) const;
  std::vector<nyx::GroupRecord> list_groups() const;

 private:
  void emit_status(const std::string& text);
  void emit_message(const nyx::ChatMessage& msg, bool outgoing);
  void emit_chat_ready(const std::string& peer_title, ConnectionVia via,
                       const std::string& peer_host = {});
  void set_mode(NodeMode mode);

  bool parse_rendezvous(std::string& host, uint16_t& port) const;
  nyx::Profile load_profile() const;

  void run_listen(bool lan_advertise);
  void run_connect_token(std::string token_hex);
  void run_connect_peer(std::string host, uint16_t port);
  void run_browse(int timeout_ms);
  void run_lan_scan(int timeout_ms);
  void run_group_hub(std::string group_id_hex);
  void run_group_join(std::string invite_hex);

  void run_direct_chat(std::unique_ptr<nyx::Connection> connection,
                       const nyx::Profile& profile, bool incoming, ConnectionVia via);

  mutable std::mutex cb_mutex_;
  StatusCallback on_status_;
  MessageCallback on_message_;
  TokenCallback on_invite_token_;
  LanPeersCallback on_lan_peers_;
  GroupInfoCallback on_group_created_;
  ChatReadyCallback on_chat_ready_;
  FileProgressCallback on_file_progress_;

  std::string profile_path_;
  std::string nickname_;
  std::string rendezvous_ = "127.0.0.1:3478";
  nyx::NetworkConfig network_config_;

  std::atomic<NodeMode> mode_{NodeMode::Idle};
  std::atomic<bool> busy_{false};
  std::atomic<bool> running_{false};

  std::thread worker_;
  std::thread discovery_thread_;
  std::atomic<bool> discovery_busy_{false};
  std::unique_ptr<nyx::MdnsLan> mdns_;
  std::unique_ptr<nyx::ChatService> chat_;
  std::unique_ptr<nyx::FileTransferService> files_;
  std::unique_ptr<nyx::GroupHub> group_hub_;
  std::unique_ptr<nyx::GroupMemberService> group_member_;
  std::unique_ptr<nyx::Connection> connection_;
  nyx::FileIndex file_index_;
  nyx::GroupId share_scope_group_{};
};

}  // namespace nyx_app
