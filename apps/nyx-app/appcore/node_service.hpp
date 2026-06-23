#pragma once

/** @file node_service.hpp
 *  AppCore без Qt: listen/connect/chat/files/groups для nyx-app и тестов.
 */

#include "connection_label.hpp"
#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/identity.hpp"
#include "nyx/mdns.hpp"
#include "nyx/messaging.hpp"
#include "nyx/conversation.hpp"
#include "nyx/network_config.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
  using ChatReadyCallback = std::function<void(const std::string& title,
                                                 const std::string& connection_label,
                                                 nyx::ConversationKind kind,
                                                 const std::string& ref_id_hex)>;

  NodeService();
  ~NodeService();

  NodeService(const NodeService&) = delete;
  NodeService& operator=(const NodeService&) = delete;

  void set_profile_path(std::string path);
  void set_nickname(std::string nickname);
  void set_rendezvous(std::string addr);
  bool set_rendezvous_list(const std::string& csv);
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
  using FileIndexProgressCallback =
      std::function<void(const std::string& path, int files_scanned, bool finished)>;
  using RemoteFilesCallback = std::function<void(const std::vector<nyx::FileEntry>&)>;
  using FileAccessSyncCallback = std::function<void()>;
  void set_on_file_progress(FileProgressCallback cb);
  void set_on_file_index_progress(FileIndexProgressCallback cb);
  void set_on_remote_files(RemoteFilesCallback cb);
  void set_on_file_access_sync(FileAccessSyncCallback cb);
  void set_on_mode(std::function<void(NodeMode)> cb);
  using SessionEndedCallback = std::function<void()>;
  void set_on_session_ended(SessionEndedCallback cb);

  NodeMode mode() const { return mode_.load(); }
  bool busy() const { return busy_.load(); }

  nyx::Profile profile() const;

  bool start_listen(bool lan_advertise = true);
  bool start_connect_token(const std::string& token_hex);
  bool start_connect_peer(const std::string& host, uint16_t port);
  bool start_browse(int timeout_ms = 3000);
  bool scan_lan_peers(int timeout_ms = 2000);

  bool create_group(const std::string& name);
  bool delete_group(const std::string& group_id_hex);
  bool remove_group_member(const std::string& group_id_hex, const std::string& user_id_hex);
  bool auto_start_owned_hub() const { return network_config_.auto_start_owned_hub; }
  void set_auto_start_owned_hub(bool enabled);
  bool start_group_hub(const std::string& group_id_hex);
  bool start_group_join(const std::string& invite_hex);

  void stop();

  /** Перечитывает индекс и ACL после unlock аккаунта (data_dir сменился). */
  void reload_account_data();
  /** Сбрасывает данные аккаунта в памяти при signOut. */
  void clear_account_data();

  std::string load_files_scope_group_id() const;
  void save_files_scope_group_id(const std::string& scope_group_id_hex) const;
  std::string load_files_selected_root() const;
  void save_files_selected_root(const std::string& root_path) const;

  bool send_message(const std::string& text);
  bool send_bye(const std::string& reason);
  bool index_folder(const std::string& path, const std::string& scope_group_id_hex = {});
  bool remove_share_root(const std::string& path, const std::string& scope_group_id_hex = {});
  bool rescan_share_root(const std::string& path, const std::string& scope_group_id_hex = {});
  int file_count_in_root(const std::string& root_path) const;
  bool request_remote_files();
  bool request_file_access_policy();
  bool download_file(const std::string& hash_hex, const std::string& dest_path = {});
  /** Ставит в очередь скачивание всех файлов под folder_rel (включая подпапки). */
  std::size_t enqueue_folder_downloads(const std::string& root_path,
                                       const std::string& folder_rel,
                                       const std::string& dest_dir = {});
  bool send_file(const std::string& path_or_hash);
  /** Можно запрашивать файлы peer (не hub-only режим). */
  bool can_request_remote_files() const;
  std::string file_exchange_hint() const;
  std::vector<nyx::FileEntry> local_files_for_scope(const std::string& scope_group_id_hex) const;
  std::vector<nyx::ShareRoot> share_roots_for_scope(const std::string& scope_group_id_hex) const;
  std::vector<nyx::FileEntry> remote_files() const;

  /** Права текущего пользователя в области. root_path и relative_path — для ACL на объект. */
  uint32_t my_file_permissions(const std::string& scope_group_id_hex,
                               const std::string& root_path = {},
                               const std::string& relative_path = {}) const;
  nyx::GroupFileAccess file_access_policy(const std::string& scope_group_id_hex);
  bool set_member_file_role(const std::string& scope_group_id_hex,
                            const std::string& user_id_hex, const std::string& role_id);
  bool upsert_file_role(const std::string& scope_group_id_hex, const nyx::FileRole& role);
  bool remove_file_role(const std::string& scope_group_id_hex, const std::string& role_id);
  bool set_root_member_file_role(const std::string& scope_group_id_hex,
                                 const std::string& root_path,
                                 const std::string& user_id_hex,
                                 const std::string& role_id);
  bool set_path_member_file_role(const std::string& scope_group_id_hex,
                                 const std::string& root_path,
                                 const std::string& relative_path,
                                 const std::string& user_id_hex,
                                 const std::string& role_id);
  bool set_path_direct_file_permissions(const std::string& scope_group_id_hex,
                                        const std::string& root_path,
                                        const std::string& relative_path,
                                        const std::string& user_id_hex,
                                        uint32_t permissions);
  bool set_path_role(const std::string& scope_group_id_hex, const std::string& root_path,
                     const std::string& relative_path, const std::string& role_id);
  bool upsert_permission_preset(const std::string& scope_group_id_hex,
                                const nyx::FilePermissionPreset& preset);
  bool remove_permission_preset(const std::string& scope_group_id_hex,
                                const std::string& preset_id);

  std::vector<nyx::ShareRoot> all_share_roots() const;
  std::vector<nyx::FileEntry> local_files_at_root(const std::string& share_root_path,
                                                  const std::string& parent_rel) const;

  /** Публикует индекс поля на hub (участник или hub после индексации). */
  void publish_field_index();

  std::vector<nyx::StoredMessage> chat_history(std::size_t count = 50) const;
  std::vector<nyx::GroupRecord> list_groups() const;
  /** Hex id поля, если сейчас запущен hub; иначе пустая строка. */
  std::string running_group_hub_id_hex() const;

 private:
  void emit_status(const std::string& text);
  void emit_message(const nyx::ChatMessage& msg, bool outgoing);
  void emit_session_ended();
  void emit_chat_ready(const std::string& title, ConnectionVia via,
                       const std::string& peer_host, nyx::ConversationKind kind,
                       const std::string& ref_id_hex);
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

  /** Копирует roster активной сессии поля (только worker). */
  void sync_live_group_from_session();
  void set_live_group_snapshot(nyx::GroupRecord rec);
  void clear_live_group_snapshot();

  void run_direct_chat(std::unique_ptr<nyx::Connection> connection,
                       const nyx::Profile& profile, bool incoming, ConnectionVia via);

  void wire_file_transfer(nyx::FileTransferService& files);
  void drain_file_download_queue();
  void try_pump_download_queue();
  void after_file_access_changed(const std::string& scope_group_id_hex);
  bool try_apply_file_access_policy(const nyx::ByteBuffer& payload);
  std::string resolve_share_root_path(const std::string& root_path) const;
  nyx::GroupId scope_from_hex(const std::string& scope_group_id_hex) const;

  bool session_blocks_new_work() const;

  mutable std::mutex cb_mutex_;
  mutable std::mutex live_group_mutex_;
  std::optional<nyx::GroupRecord> live_group_snapshot_;
  StatusCallback on_status_;
  MessageCallback on_message_;
  TokenCallback on_invite_token_;
  LanPeersCallback on_lan_peers_;
  GroupInfoCallback on_group_created_;
  ChatReadyCallback on_chat_ready_;
  FileProgressCallback on_file_progress_;
  FileIndexProgressCallback on_file_index_progress_;
  RemoteFilesCallback on_remote_files_;
  FileAccessSyncCallback on_file_access_sync_;
  std::function<void(NodeMode)> on_mode_;
  SessionEndedCallback on_session_ended_;

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
  mutable nyx::FileAccessStore file_access_;
  nyx::GroupId share_scope_group_{};

  struct FileDownloadRequest {
    std::string hash_hex;
    std::string dest_path;
  };
  std::deque<FileDownloadRequest> file_download_queue_;
  std::mutex file_download_queue_mutex_;
};

}  // namespace nyx_app
