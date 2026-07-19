#pragma once

/** @file node_service.hpp
 *  AppCore без Qt: multi-session listen/connect/chat/files/groups.
 */

#include "connection_label.hpp"
#include "session_types.hpp"
#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/connection.hpp"
#include "nyx/conversation.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/file_transfer.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/identity.hpp"
#include "nyx/mdns.hpp"
#include "nyx/messaging.hpp"
#include "nyx/network_config.hpp"
#include "nyx/session_intent.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nyx_app {

struct UiMessage {
  uint64_t message_id = 0;
  uint64_t timestamp_ms = 0;
  std::string author;
  /** Hex user id автора (для /me и @). */
  std::string author_user_id;
  std::string text;
  bool outgoing = false;
  /** pending | delivered | failed | пусто */
  std::string delivery;
  std::string session_id;
  std::string chat_key;
};

/** Legacy aggregate mode (для listening / status). */
enum class NodeMode {
  Idle,
  Listening,
  ChatDirect,
  GroupHub,
  GroupMember,
};

/** Оркестратор сетевых сценариев: несколько параллельных сессий. */
class NodeService {
 public:
  using StatusCallback = std::function<void(const std::string&)>;
  using MessageCallback = std::function<void(const UiMessage&)>;
  using DeliveryCallback =
      std::function<void(const std::string& session_id, uint64_t message_id, bool delivered)>;
  using TokenCallback = std::function<void(const std::string& token_hex)>;
  using LanPeersCallback = std::function<void(const std::vector<nyx::LanPeer>&)>;
  using GroupInfoCallback = std::function<void(const std::string& group_id_hex,
                                               const std::string& invite_hex)>;
  using ChatReadyCallback = std::function<void(const std::string& session_id,
                                               const std::string& title,
                                               const std::string& connection_label,
                                               nyx::ConversationKind kind,
                                               const std::string& ref_id_hex)>;
  using SessionEndedCallback = std::function<void(const std::string& session_id)>;
  using SessionsChangedCallback = std::function<void()>;

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
  void set_on_delivery(DeliveryCallback cb);
  void set_on_invite_token(TokenCallback cb);
  void set_on_lan_peers(LanPeersCallback cb);
  void set_on_group_created(GroupInfoCallback cb);
  /** Мете поля обновилась у участника (push от hub). */
  void set_on_group_meta_changed(SessionsChangedCallback cb);
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
  void set_on_avatars_changed(SessionsChangedCallback cb);
  void set_on_mode(std::function<void(NodeMode)> cb);
  void set_on_session_ended(SessionEndedCallback cb);
  void set_on_sessions_changed(SessionsChangedCallback cb);

  NodeMode mode() const;
  bool busy() const;
  /** Число live-сессий (без inbox). */
  std::size_t live_session_count() const;
  std::vector<SessionInfo> list_sessions() const;
  SessionState session_state(const std::string& session_id) const;
  bool is_session_live(const std::string& session_id) const;
  /** Live или Connecting — reconnect не должен перезапускать такую сессию. */
  bool is_session_up(const std::string& session_id) const;
  std::string active_session_id() const;
  void set_active_session(const std::string& session_id);

  nyx::Profile profile() const;

  bool start_listen(bool lan_advertise = true);
  bool start_dm_inbox();
  /** @param quiet_ui — фоновый reconnect без «переподключение» в UI. */
  bool start_connect_token(const std::string& token_hex, bool quiet_ui = false);
  bool start_connect_peer(const std::string& host, uint16_t port);
  bool start_browse(int timeout_ms = 3000);
  bool scan_lan_peers(int timeout_ms = 2000);

  bool create_group(const std::string& name);
  bool update_group_meta(const std::string& group_id_hex, const std::string& description,
                         const std::string& direction, const std::string& tags,
                         bool public_listed);
  bool delete_group(const std::string& group_id_hex);
  bool remove_group_member(const std::string& group_id_hex, const std::string& user_id_hex);
  bool auto_start_owned_hub() const { return network_config_.auto_start_owned_hub; }
  void set_auto_start_owned_hub(bool enabled);
  bool start_group_hub(const std::string& group_id_hex);
  /** @param quiet_ui — не показывать «переподключение» в списке (фоновый probe). */
  bool start_group_join(const std::string& invite_hex, bool quiet_ui = false);
  /** Сброс счётчика фоновых ретраев (ручной join / «подключить»). */
  void reset_join_reconnect_budget(const std::string& chat_key);

  /** Останавливает одну сессию (или active, если id пуст). */
  bool stop_session(const std::string& session_id = {});
  /** Останавливает все сессии (signOut / выход). */
  void stop();

  /** Поднимает owned hubs, inbox, enabled intents. */
  void auto_reconnect_all();
  /** ensureSession: hub/join/DM по ключу чата. */
  bool ensure_session(const std::string& chat_key);
  /** При входе: включить intent и поднять hub всех своих полей. */
  void ensure_owned_hubs_running();
  /** Включает auto-reconnect для чата (после попытки join / до появления hub). */
  void enable_session_intent(nyx::SessionIntent intent);
  /** Выключает intent («Отключиться») — фоновый reconnect не поднимает чат. */
  void mark_session_disconnected(const std::string& chat_key);
  bool is_session_intent_enabled(const std::string& chat_key) const;

  void reload_account_data();
  void clear_account_data();

  std::string load_files_scope_group_id() const;
  void save_files_scope_group_id(const std::string& scope_group_id_hex) const;
  std::string load_files_selected_root() const;
  void save_files_selected_root(const std::string& root_path) const;

  /** Отправка в указанную сессию (или в active, если session_id пуст). */
  bool send_message(const std::string& text, const std::string& session_id = {});
  bool send_bye(const std::string& reason);
  bool index_folder(const std::string& path, const std::string& scope_group_id_hex = {});
  bool remove_share_root(const std::string& path, const std::string& scope_group_id_hex = {});
  bool rescan_share_root(const std::string& path, const std::string& scope_group_id_hex = {});
  int file_count_in_root(const std::string& root_path) const;
  bool request_remote_files();
  /** Запрос каталога: scope — group hex; root/parent пустые = только share-корни. */
  bool request_remote_files_at(const std::string& root_path, const std::string& parent_rel);
  bool request_remote_files_at(const std::string& scope_group_id_hex, const std::string& root_path,
                               const std::string& parent_rel);
  bool request_file_access_policy();
  bool download_file(const std::string& hash_hex, const std::string& dest_path = {});
  std::size_t enqueue_folder_downloads(const std::string& root_path,
                                       const std::string& folder_rel,
                                       const std::string& dest_dir = {});
  bool send_file(const std::string& path_or_hash);
  bool can_request_remote_files() const;
  /** Сессия для обмена файлами в области (group:<hex> или active). */
  std::string file_exchange_session_id(const std::string& scope_group_id_hex) const;
  std::string file_exchange_hint() const;
  std::vector<nyx::FileEntry> local_files_for_scope(const std::string& scope_group_id_hex) const;
  std::vector<nyx::ShareRoot> share_roots_for_scope(const std::string& scope_group_id_hex) const;
  std::vector<nyx::FileEntry> remote_files() const;

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

  void publish_field_index();

  std::vector<nyx::StoredMessage> chat_history(std::size_t count = 50) const;
  std::vector<nyx::GroupRecord> list_groups() const;
  std::string running_group_hub_id_hex() const;
  bool is_group_hub_running(const std::string& group_id_hex) const;
  std::string dm_inbox_token_hex() const;

 private:
  struct FileDownloadRequest {
    std::string hash_hex;
    std::string dest_path;
  };

  struct NetSession {
    std::string id;
    SessionKind kind = SessionKind::Idle;
    std::atomic<SessionState> state{SessionState::Idle};
    std::atomic<bool> running{false};
    /** Connecting, но UI показывает offline (тихий фоновый probe). */
    std::atomic<bool> quiet_ui{false};
    /** Уже был Live в этой сессии — обрыв эфира не считается failed join. */
    std::atomic<bool> ever_live{false};
    std::thread worker;
    std::unique_ptr<nyx::Connection> connection;
    std::unique_ptr<nyx::ChatService> chat;
    std::unique_ptr<nyx::FileTransferService> files;
    std::unique_ptr<nyx::GroupHub> group_hub;
    std::unique_ptr<nyx::GroupMemberService> group_member;
    std::unique_ptr<nyx::MdnsLan> mdns;
    nyx::GroupId share_scope{};
    std::string title;
    std::string ref_id_hex;
    std::deque<FileDownloadRequest> download_queue;
    std::mutex download_mutex;

    struct AvatarRx {
      nyx::FileHash hash{};
      uint64_t size = 0;
      std::string mime;
      nyx::ByteBuffer data;
    };
    std::optional<AvatarRx> avatar_rx;
    nyx::UserId avatar_peer{};

    NetSession() = default;
    NetSession(const NetSession&) = delete;
    NetSession& operator=(const NetSession&) = delete;
    /** Иначе joinable std::thread в деструкторе вызывает std::terminate. */
    ~NetSession() {
      if (!worker.joinable()) return;
      running.store(false);
      if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
      } else {
        worker.join();
      }
    }
  };

  void emit_status(const std::string& text);
  void emit_message(const std::shared_ptr<NetSession>& session, const nyx::ChatMessage& msg,
                    bool outgoing, const std::string& delivery = {});
  void emit_delivery(const std::shared_ptr<NetSession>& session, uint64_t message_id,
                     bool delivered);
  void emit_session_ended(const std::string& session_id);
  void emit_sessions_changed();
  void emit_chat_ready(const std::shared_ptr<NetSession>& session, const std::string& title,
                       ConnectionVia via, const std::string& peer_host,
                       nyx::ConversationKind kind, const std::string& ref_id_hex);
  void set_mode(NodeMode mode);

  bool parse_rendezvous(std::string& host, uint16_t& port) const;
  nyx::Profile load_profile() const;

  std::shared_ptr<NetSession> find_session_locked(const std::string& id) const;
  std::shared_ptr<NetSession> active_session_locked() const;
  std::shared_ptr<NetSession> active_session() const;
  std::shared_ptr<NetSession> find_session(const std::string& id) const;
  std::shared_ptr<NetSession> create_session(const std::string& id, SessionKind kind);
  void finish_session(const std::shared_ptr<NetSession>& session, SessionState final_state);
  void stop_session_locked(const std::shared_ptr<NetSession>& session);
  /** Останавливает worker без блокирующего join (безопасно из UI-потока). */
  void abandon_session_worker(const std::shared_ptr<NetSession>& session);

  void run_listen(std::shared_ptr<NetSession> session, bool lan_advertise);
  void run_dm_inbox(std::shared_ptr<NetSession> session);
  void run_connect_token(std::shared_ptr<NetSession> session, std::string token_hex);
  void run_connect_peer(std::shared_ptr<NetSession> session, std::string host, uint16_t port);
  void run_browse(int timeout_ms);
  void run_lan_scan(int timeout_ms);
  void run_group_hub(std::shared_ptr<NetSession> session, std::string group_id_hex);
  void run_group_join(std::shared_ptr<NetSession> session, std::string invite_hex);
  void run_direct_chat(std::shared_ptr<NetSession> session,
                       std::unique_ptr<nyx::Connection> connection, const nyx::Profile& profile,
                       bool incoming, ConnectionVia via);

  void sync_live_group_from_session(const std::shared_ptr<NetSession>& session);
  void set_live_group_snapshot(const nyx::GroupId& id, nyx::GroupRecord rec);
  void clear_live_group_snapshot(const nyx::GroupId& id);

  void wire_file_transfer(const std::shared_ptr<NetSession>& session,
                          nyx::FileTransferService& files);
  void drain_file_download_queue(const std::shared_ptr<NetSession>& session);
  void try_pump_download_queue();
  void after_file_access_changed(const std::string& scope_group_id_hex);
  bool try_apply_file_access_policy(const nyx::ByteBuffer& payload);

  void request_missing_avatars(nyx::Connection& conn, const nyx::UserId& peer,
                               const std::vector<nyx::FileHash>& hashes);
  void sync_avatars_after_hello(const std::shared_ptr<NetSession>& session,
                                const nyx::HelloMessage& peer);
  bool handle_avatar_bulk(const std::shared_ptr<NetSession>& session,
                          const nyx::ByteBuffer& payload);
  std::string resolve_share_root_path(const std::string& root_path) const;
  nyx::GroupId scope_from_hex(const std::string& scope_group_id_hex) const;
  void remember_intent_for_session(const std::shared_ptr<NetSession>& session,
                                   const std::string& invite_hex = {});

  /** Бюджет видимых join-ретраев чужого поля; дальше — тихие редкие probe. */
  struct JoinReconnectBudget {
    int failures = 0;
    int64_t next_attempt_ms = 0;
  };
  static constexpr int kMaxVisibleJoinRetries = 3;
  static constexpr int64_t kQuietJoinProbeIntervalMs = 60'000;

  static int64_t steady_now_ms();
  void note_join_reconnect_failure(const std::string& chat_key);
  void clear_join_reconnect_budget(const std::string& chat_key);
  SessionState ui_session_state(const std::shared_ptr<NetSession>& session) const;

  mutable std::mutex cb_mutex_;
  mutable std::mutex live_group_mutex_;
  mutable std::mutex sessions_mutex_;
  mutable std::mutex join_reconnect_mutex_;
  std::map<nyx::GroupId, nyx::GroupRecord> live_group_snapshots_;
  std::map<std::string, std::shared_ptr<NetSession>> sessions_;
  std::unordered_map<std::string, JoinReconnectBudget> join_reconnect_;
  std::string active_session_id_;

  StatusCallback on_status_;
  MessageCallback on_message_;
  DeliveryCallback on_delivery_;
  TokenCallback on_invite_token_;
  LanPeersCallback on_lan_peers_;
  GroupInfoCallback on_group_created_;
  SessionsChangedCallback on_group_meta_changed_;
  ChatReadyCallback on_chat_ready_;
  FileProgressCallback on_file_progress_;
  FileIndexProgressCallback on_file_index_progress_;
  RemoteFilesCallback on_remote_files_;
  FileAccessSyncCallback on_file_access_sync_;
  SessionsChangedCallback on_avatars_changed_;
  std::function<void(NodeMode)> on_mode_;
  SessionEndedCallback on_session_ended_;
  SessionsChangedCallback on_sessions_changed_;

  std::string profile_path_;
  std::string nickname_;
  std::string rendezvous_ = "127.0.0.1:3478";
  nyx::NetworkConfig network_config_;

  std::atomic<NodeMode> mode_{NodeMode::Idle};
  std::thread discovery_thread_;
  std::atomic<bool> discovery_busy_{false};

  nyx::FileIndex file_index_;
  /** Кэш каталога «Ресурсы» для локального hub (корни + подгруженные уровни). */
  std::vector<nyx::FileEntry> hub_remote_catalog_;
  mutable nyx::FileAccessStore file_access_;
  nyx::SessionIntentStore intent_store_;
};

}  // namespace nyx_app
