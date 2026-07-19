#include "node_service.hpp"

#include "nyx/account_store.hpp"
#include "nyx/log.hpp"
#include "nyx/message_store.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nyx_app {

NodeService::NodeService() {
  nyx::log_init();
}

NodeService::~NodeService() { stop(); }

void NodeService::set_profile_path(std::string path) { profile_path_ = std::move(path); }
void NodeService::set_nickname(std::string nickname) {
  nickname_ = std::move(nickname);
  if (nyx::active_account_id().empty()) return;
  std::string err;
  nyx::update_session_nickname(nickname_, &err);
}

void NodeService::set_rendezvous(std::string addr) {
  rendezvous_ = std::move(addr);
  nyx::NetworkConfig tmp;
  if (nyx::NetworkConfig::parse_rendezvous_list(rendezvous_, tmp)) {
    network_config_.rendezvous_servers = tmp.rendezvous_servers;
  }
}

bool NodeService::set_rendezvous_list(const std::string& csv) {
  nyx::NetworkConfig tmp;
  if (!nyx::NetworkConfig::parse_rendezvous_list(csv, tmp)) return false;
  network_config_.rendezvous_servers = tmp.rendezvous_servers;
  rendezvous_ = network_config_.rendezvous_list_string();
  return true;
}

void NodeService::set_discovery_mode(int mode) {
  if (mode == 1)
    network_config_.mode = nyx::DiscoveryMode::LanOnly;
  else if (mode == 2)
    network_config_.mode = nyx::DiscoveryMode::Internet;
  else
    network_config_.mode = nyx::DiscoveryMode::Auto;
}

bool NodeService::save_network_config() { return network_config_.save(); }

bool NodeService::load_network_config() {
  if (!network_config_.load()) return false;
  rendezvous_ = network_config_.rendezvous_list_string();
  if (rendezvous_.empty()) {
    const auto p = network_config_.primary_rendezvous();
    rendezvous_ = p.host + ':' + std::to_string(p.port);
  }
  return true;
}

std::string NodeService::rendezvous_list_string() const {
  return network_config_.rendezvous_list_string();
}

bool NodeService::test_rendezvous(const std::string& host, uint16_t port) {
  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) return false;
  nyx::RendezvousPool pool(std::move(socket));
  nyx::RendezvousServer srv;
  srv.host = host;
  srv.port = port;
  pool.set_servers({srv});
  return pool.probe_server(srv, 2500);
}

void NodeService::set_on_status(StatusCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_status_ = std::move(cb);
}
void NodeService::set_on_delivery(DeliveryCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_delivery_ = std::move(cb);
}

void NodeService::set_on_message(MessageCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_message_ = std::move(cb);
}
void NodeService::set_on_invite_token(TokenCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_invite_token_ = std::move(cb);
}
void NodeService::set_on_lan_peers(LanPeersCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_lan_peers_ = std::move(cb);
}
void NodeService::set_on_group_created(GroupInfoCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_group_created_ = std::move(cb);
}

void NodeService::set_on_group_meta_changed(SessionsChangedCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_group_meta_changed_ = std::move(cb);
}
void NodeService::set_on_chat_ready(ChatReadyCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_chat_ready_ = std::move(cb);
}

void NodeService::set_on_file_progress(FileProgressCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_file_progress_ = std::move(cb);
}

void NodeService::reload_account_data() {
  file_index_.reload();
  file_access_.load();
  intent_store_ = nyx::SessionIntentStore{};
  intent_store_.load();
  {
    std::lock_guard lock(join_reconnect_mutex_);
    join_reconnect_.clear();
  }
}

void NodeService::clear_account_data() {
  file_index_.clear();
  file_access_.clear();
  intent_store_ = nyx::SessionIntentStore{};
  {
    std::lock_guard lock(join_reconnect_mutex_);
    join_reconnect_.clear();
  }
}

int64_t NodeService::steady_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NodeService::note_join_reconnect_failure(const std::string& chat_key) {
  if (chat_key.empty()) return;
  std::lock_guard lock(join_reconnect_mutex_);
  auto& bud = join_reconnect_[chat_key];
  if (bud.failures < kMaxVisibleJoinRetries)
    ++bud.failures;
  if (bud.failures >= kMaxVisibleJoinRetries)
    bud.next_attempt_ms = steady_now_ms() + kQuietJoinProbeIntervalMs;
}

void NodeService::clear_join_reconnect_budget(const std::string& chat_key) {
  if (chat_key.empty()) return;
  std::lock_guard lock(join_reconnect_mutex_);
  join_reconnect_.erase(chat_key);
}

void NodeService::reset_join_reconnect_budget(const std::string& chat_key) {
  clear_join_reconnect_budget(chat_key);
}

SessionState NodeService::ui_session_state(const std::shared_ptr<NetSession>& session) const {
  if (!session) return SessionState::Idle;
  const auto st = session->state.load();
  if (st == SessionState::Connecting && session->quiet_ui.load())
    return SessionState::Offline;
  return st;
}

namespace {

std::string files_ui_state_path() { return nyx::data_dir() + "/files_ui.json"; }

std::optional<std::string> json_get_string_value(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"') break;
    if (c == '\\' && i < json.size())
      out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

}  // namespace

std::string NodeService::load_files_scope_group_id() const {
  std::ifstream in(files_ui_state_path());
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  if (auto gid = json_get_string_value(ss.str(), "scope_group_id")) return *gid;
  return {};
}

std::string NodeService::load_files_selected_root() const {
  std::ifstream in(files_ui_state_path());
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  if (auto root = json_get_string_value(ss.str(), "selected_root")) return *root;
  return {};
}

void NodeService::save_files_scope_group_id(const std::string& scope_group_id_hex) const {
  nyx::ensure_data_dir();
  const std::string selected = load_files_selected_root();
  std::ofstream out(files_ui_state_path(), std::ios::trunc);
  if (!out) return;
  out << "{\"scope_group_id\":\"" << scope_group_id_hex << "\",\"selected_root\":\""
      << selected << "\"}\n";
}

void NodeService::save_files_selected_root(const std::string& root_path) const {
  nyx::ensure_data_dir();
  const std::string scope = load_files_scope_group_id();
  std::ofstream out(files_ui_state_path(), std::ios::trunc);
  if (!out) return;
  std::string escaped;
  for (char c : root_path) {
    if (c == '\\')
      escaped += "\\\\";
    else if (c == '"')
      escaped += "\\\"";
    else
      escaped += c;
  }
  out << "{\"scope_group_id\":\"" << scope << "\",\"selected_root\":\"" << escaped << "\"}\n";
}

void NodeService::set_on_file_index_progress(FileIndexProgressCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_file_index_progress_ = std::move(cb);
}

void NodeService::set_on_remote_files(RemoteFilesCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_remote_files_ = std::move(cb);
}

void NodeService::set_on_avatars_changed(SessionsChangedCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_avatars_changed_ = std::move(cb);
}

void NodeService::set_on_file_access_sync(FileAccessSyncCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_file_access_sync_ = std::move(cb);
}

void NodeService::set_on_mode(std::function<void(NodeMode)> cb) {
  std::lock_guard lock(cb_mutex_);
  on_mode_ = std::move(cb);
}

void NodeService::set_on_session_ended(SessionEndedCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_session_ended_ = std::move(cb);
}

void NodeService::set_on_sessions_changed(SessionsChangedCallback cb) {
  std::lock_guard lock(cb_mutex_);
  on_sessions_changed_ = std::move(cb);
}

nyx::Profile NodeService::profile() const { return load_profile(); }

nyx::Profile NodeService::load_profile() const {
  nyx::Profile active;
  if (nyx::active_profile(active)) return active;
  const std::string path =
      profile_path_.empty() ? nyx::default_profile_path() : profile_path_;
  return nyx::load_or_create_profile(path, nickname_);
}

bool NodeService::parse_rendezvous(std::string& host, uint16_t& port) const {
  const auto primary = network_config_.primary_rendezvous();
  host = primary.host;
  port = primary.port;
  if (!host.empty() && port != 0) return true;
  return nyx::parse_host_port(rendezvous_, host, port);
}

void NodeService::emit_status(const std::string& text) {
  nyx::log_info(text);
  StatusCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_status_;
  }
  if (cb) cb(text);
}

void NodeService::emit_message(const std::shared_ptr<NetSession>& session,
                               const nyx::ChatMessage& msg, bool outgoing,
                               const std::string& delivery) {
  MessageCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_message_;
  }
  if (!cb) return;
  UiMessage ui;
  ui.message_id = msg.id;
  ui.timestamp_ms = msg.timestamp_ms;
  ui.author = msg.author;
  ui.author_user_id = nyx::to_hex(msg.author_id.data(), msg.author_id.size());
  ui.text = msg.text;
  ui.outgoing = outgoing;
  ui.delivery = delivery;
  if (session) {
    ui.session_id = session->id;
    ui.chat_key = session->id;
  }
  cb(ui);
}

void NodeService::emit_delivery(const std::shared_ptr<NetSession>& session, uint64_t message_id,
                                bool delivered) {
  DeliveryCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_delivery_;
  }
  if (!cb || message_id == 0) return;
  cb(session ? session->id : std::string{}, message_id, delivered);
}

void NodeService::emit_session_ended(const std::string& session_id) {
  SessionEndedCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_session_ended_;
  }
  if (cb) cb(session_id);
  emit_sessions_changed();
}

void NodeService::emit_sessions_changed() {
  SessionsChangedCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_sessions_changed_;
  }
  if (cb) cb();
}

void NodeService::emit_chat_ready(const std::shared_ptr<NetSession>& session,
                                  const std::string& title, ConnectionVia via,
                                  const std::string& peer_host, nyx::ConversationKind kind,
                                  const std::string& ref_id_hex) {
  if (session) {
    session->title = title;
    session->ref_id_hex = ref_id_hex;
    session->quiet_ui.store(false);
    session->ever_live.store(true);
    session->state.store(SessionState::Live);
    clear_join_reconnect_budget(session->id);
    if (!ref_id_hex.empty())
      clear_join_reconnect_budget(make_group_session_id(ref_id_hex));
  }
  ChatReadyCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_chat_ready_;
  }
  if (cb) {
    cb(session ? session->id : std::string{}, title,
       nyx_app::connection_label(via, peer_host), kind, ref_id_hex);
  }
  emit_sessions_changed();
}

void NodeService::set_mode(NodeMode mode) {
  mode_.store(mode);
  std::function<void(NodeMode)> cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_mode_;
  }
  if (cb) cb(mode);
}

std::shared_ptr<NodeService::NetSession> NodeService::find_session_locked(
    const std::string& id) const {
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) return nullptr;
  return it->second;
}

std::shared_ptr<NodeService::NetSession> NodeService::active_session_locked() const {
  if (!active_session_id_.empty()) {
    if (auto s = find_session_locked(active_session_id_)) return s;
  }
  for (const auto& [id, s] : sessions_) {
    if (!s) continue;
    if (s->kind == SessionKind::DmInbox) continue;
    if (s->state.load() == SessionState::Live) return s;
  }
  return nullptr;
}

std::shared_ptr<NodeService::NetSession> NodeService::active_session() const {
  std::lock_guard lock(sessions_mutex_);
  return active_session_locked();
}

std::shared_ptr<NodeService::NetSession> NodeService::find_session(
    const std::string& id) const {
  std::lock_guard lock(sessions_mutex_);
  return find_session_locked(id);
}

std::shared_ptr<NodeService::NetSession> NodeService::create_session(const std::string& id,
                                                                      SessionKind kind) {
  auto session = std::make_shared<NetSession>();
  session->id = id;
  session->kind = kind;
  session->state.store(SessionState::Connecting);
  session->running.store(true);
  sessions_[id] = session;
  return session;
}

void NodeService::finish_session(const std::shared_ptr<NetSession>& session,
                                 SessionState final_state) {
  if (!session) return;
  if (final_state == SessionState::Offline && !session->ever_live.load()) {
    if (session->kind == SessionKind::GroupMember) {
      const std::string key = !session->ref_id_hex.empty()
                                  ? make_group_session_id(session->ref_id_hex)
                                  : session->id;
      note_join_reconnect_failure(key);
    } else if (session->kind == SessionKind::Direct) {
      const std::string key = !session->ref_id_hex.empty()
                                  ? make_dm_session_id(session->ref_id_hex)
                                  : session->id;
      note_join_reconnect_failure(key);
    }
  }
  session->chat.reset();
  session->files.reset();
  session->group_hub.reset();
  session->group_member.reset();
  session->connection.reset();
  session->mdns.reset();
  session->running.store(false);
  session->quiet_ui.store(false);
  session->state.store(final_state);
  // Не detach здесь: параллельный join() из UI даёт data race / краш.
  // Поток остаётся joinable до join() снаружи или ~NetSession.
  {
    std::lock_guard lock(sessions_mutex_);
    if (active_session_id_ == session->id) {
      bool still_live = false;
      for (const auto& [id, s] : sessions_) {
        if (s && s->id != session->id && s->state.load() == SessionState::Live &&
            s->kind != SessionKind::DmInbox) {
          active_session_id_ = id;
          still_live = true;
          break;
        }
      }
      if (!still_live) active_session_id_.clear();
    }
  }
  set_mode(mode());
  emit_session_ended(session->id);
}

void NodeService::stop_session_locked(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  session->running.store(false);
}

void NodeService::abandon_session_worker(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  session->running.store(false);
  // Не join() из UI: lookup/reconnect может держать поток секунды → подвисание и краш.
  if (session->worker.joinable() &&
      session->worker.get_id() != std::this_thread::get_id()) {
    session->worker.detach();
  }
}

NodeMode NodeService::mode() const {
  std::lock_guard lock(sessions_mutex_);
  if (auto inbox = find_session_locked(kDmInboxSessionId)) {
    if (inbox->state.load() == SessionState::Live ||
        inbox->state.load() == SessionState::Connecting) {
      return NodeMode::Listening;
    }
  }
  if (auto active = active_session_locked()) {
    switch (active->kind) {
      case SessionKind::Direct:
        return NodeMode::ChatDirect;
      case SessionKind::GroupHub:
        return NodeMode::GroupHub;
      case SessionKind::GroupMember:
        return NodeMode::GroupMember;
      case SessionKind::DmInbox:
        return NodeMode::Listening;
      default:
        break;
    }
  }
  for (const auto& [id, s] : sessions_) {
    if (!s) continue;
    if (s->state.load() != SessionState::Live) continue;
    if (s->kind == SessionKind::GroupHub) return NodeMode::GroupHub;
    if (s->kind == SessionKind::GroupMember) return NodeMode::GroupMember;
    if (s->kind == SessionKind::Direct) return NodeMode::ChatDirect;
  }
  return NodeMode::Idle;
}

bool NodeService::busy() const {
  std::lock_guard lock(sessions_mutex_);
  for (const auto& [id, s] : sessions_) {
    if (s && s->state.load() == SessionState::Connecting) return true;
  }
  return false;
}

std::size_t NodeService::live_session_count() const {
  std::lock_guard lock(sessions_mutex_);
  std::size_t n = 0;
  for (const auto& [id, s] : sessions_) {
    if (!s || s->kind == SessionKind::DmInbox) continue;
    if (s->state.load() == SessionState::Live) ++n;
  }
  return n;
}

std::vector<SessionInfo> NodeService::list_sessions() const {
  std::lock_guard lock(sessions_mutex_);
  std::vector<SessionInfo> out;
  out.reserve(sessions_.size());
  for (const auto& [id, s] : sessions_) {
    if (!s) continue;
    SessionInfo info;
    info.id = s->id;
    info.kind = s->kind;
    info.state = ui_session_state(s);
    info.title = s->title;
    info.ref_id_hex = s->ref_id_hex;
    out.push_back(std::move(info));
  }
  return out;
}

SessionState NodeService::session_state(const std::string& session_id) const {
  auto s = find_session(session_id);
  if (!s) return SessionState::Idle;
  return ui_session_state(s);
}

bool NodeService::is_session_live(const std::string& session_id) const {
  auto s = find_session(session_id);
  return s && s->state.load() == SessionState::Live;
}

bool NodeService::is_session_up(const std::string& session_id) const {
  // Сырой state: тихий Connecting тоже «занят», иначе timer запустит второй join.
  auto s = find_session(session_id);
  if (!s) return false;
  const auto st = s->state.load();
  return st == SessionState::Live || st == SessionState::Connecting;
}

std::string NodeService::active_session_id() const {
  std::lock_guard lock(sessions_mutex_);
  return active_session_id_;
}

void NodeService::set_active_session(const std::string& session_id) {
  std::lock_guard lock(sessions_mutex_);
  active_session_id_ = session_id;
}

void NodeService::stop() {
  std::vector<std::shared_ptr<NetSession>> to_join;
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) {
      if (!s) continue;
      s->running.store(false);
      to_join.push_back(s);
    }
  }
  if (discovery_thread_.joinable()) discovery_thread_.join();
  for (auto& s : to_join) {
    if (s->worker.joinable()) {
      if (s->worker.get_id() != std::this_thread::get_id()) s->worker.join();
    }
  }
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.clear();
    active_session_id_.clear();
  }
  {
    std::lock_guard lock(live_group_mutex_);
    live_group_snapshots_.clear();
  }
  set_mode(NodeMode::Idle);
  emit_sessions_changed();
}

bool NodeService::stop_session(const std::string& session_id) {
  std::shared_ptr<NetSession> session;
  std::string id;
  {
    std::lock_guard lock(sessions_mutex_);
    id = session_id.empty() ? active_session_id_ : session_id;
    session = find_session_locked(id);
    if (!session) return false;
    id = session->id;
  }
  // Сначала выключить intent — иначе session_ended / timer успеют снова поднять сессию.
  mark_session_disconnected(id);
  if (!session->ref_id_hex.empty() && id.rfind("group:", 0) == 0) {
    mark_session_disconnected(make_group_session_id(session->ref_id_hex));
  }
  if (!session->ref_id_hex.empty() && id.rfind("dm:", 0) == 0) {
    mark_session_disconnected(make_dm_session_id(session->ref_id_hex));
  }
  abandon_session_worker(session);
  emit_sessions_changed();
  return true;
}

void NodeService::mark_session_disconnected(const std::string& chat_key) {
  if (chat_key.empty()) return;
  intent_store_.load();
  intent_store_.disable(chat_key);
  intent_store_.save();
}

bool NodeService::is_session_intent_enabled(const std::string& chat_key) const {
  nyx::SessionIntentStore store;
  store.load();
  return store.is_enabled(chat_key);
}

void NodeService::enable_session_intent(nyx::SessionIntent intent) {
  intent_store_.load();
  intent_store_.enable(std::move(intent));
  intent_store_.save();
}

void NodeService::remember_intent_for_session(const std::shared_ptr<NetSession>& session,
                                             const std::string& invite_hex) {
  if (!session || session->kind == SessionKind::DmInbox) return;
  nyx::SessionIntent intent;
  intent.ref_id_hex = session->ref_id_hex;
  intent.invite_hex = invite_hex;
  intent.enabled = true;
  if (session->kind == SessionKind::GroupHub) {
    intent.kind = nyx::SessionIntentKind::GroupHub;
    intent.key = session->ref_id_hex.empty() ? session->id
                                             : make_group_session_id(session->ref_id_hex);
  } else if (session->kind == SessionKind::GroupMember) {
    intent.kind = nyx::SessionIntentKind::GroupJoin;
    intent.key = session->ref_id_hex.empty() ? session->id
                                             : make_group_session_id(session->ref_id_hex);
  } else {
    intent.kind = nyx::SessionIntentKind::Direct;
    intent.key = session->ref_id_hex.empty() ? session->id
                                             : make_dm_session_id(session->ref_id_hex);
  }
  intent_store_.load();
  intent_store_.enable(std::move(intent));
  intent_store_.save();
}

void NodeService::set_auto_start_owned_hub(bool enabled) {
  network_config_.auto_start_owned_hub = enabled;
  save_network_config();
}

std::string NodeService::running_group_hub_id_hex() const {
  std::lock_guard lock(sessions_mutex_);
  for (const auto& [id, s] : sessions_) {
    if (!s || s->kind != SessionKind::GroupHub) continue;
    if (s->state.load() != SessionState::Live) continue;
    return s->ref_id_hex;
  }
  return {};
}

bool NodeService::is_group_hub_running(const std::string& group_id_hex) const {
  const std::string sid = make_group_session_id(group_id_hex);
  auto s = find_session(sid);
  return s && s->kind == SessionKind::GroupHub && s->state.load() == SessionState::Live;
}

std::string NodeService::dm_inbox_token_hex() const { return nyx::dm_inbox_token_hex(); }

bool NodeService::start_dm_inbox() {
  std::shared_ptr<NetSession> existing;
  {
    std::lock_guard lock(sessions_mutex_);
    existing = find_session_locked(kDmInboxSessionId);
    if (existing && (existing->state.load() == SessionState::Live ||
                     existing->state.load() == SessionState::Connecting)) {
      return true;
    }
  }
  if (existing) {
    abandon_session_worker(existing);
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(kDmInboxSessionId);
  }

  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(kDmInboxSessionId);
    session = create_session(kDmInboxSessionId, SessionKind::DmInbox);
  }
  session->worker = std::thread([this, session]() { run_dm_inbox(session); });
  set_mode(NodeMode::Listening);
  emit_sessions_changed();
  return true;
}

bool NodeService::start_connect_token(const std::string& token_hex, bool quiet_ui) {
  const std::string pending_id = "dm:pending:" + token_hex.substr(0, 8);
  {
    auto existing = find_session(pending_id);
    if (existing) {
      const auto st = existing->state.load();
      if (st == SessionState::Live || st == SessionState::Connecting) return true;
      abandon_session_worker(existing);
      std::lock_guard lock(sessions_mutex_);
      sessions_.erase(pending_id);
    }
  }
  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(pending_id);
    session = create_session(pending_id, SessionKind::Direct);
    session->quiet_ui.store(quiet_ui);
    if (active_session_id_.empty() || active_session_id_ == pending_id)
      active_session_id_ = pending_id;
  }
  session->worker =
      std::thread([this, session, token_hex]() { run_connect_token(session, token_hex); });
  emit_sessions_changed();
  return true;
}

bool NodeService::start_connect_peer(const std::string& host, uint16_t port) {
  const std::string pending_id = "dm:pending:" + host + ":" + std::to_string(port);
  {
    auto existing = find_session(pending_id);
    if (existing) {
      const auto st = existing->state.load();
      if (st == SessionState::Live || st == SessionState::Connecting) return true;
      abandon_session_worker(existing);
      std::lock_guard lock(sessions_mutex_);
      sessions_.erase(pending_id);
    }
  }
  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(pending_id);
    session = create_session(pending_id, SessionKind::Direct);
    if (active_session_id_.empty() || active_session_id_ == pending_id)
      active_session_id_ = pending_id;
  }
  session->worker =
      std::thread([this, session, host, port]() { run_connect_peer(session, host, port); });
  emit_sessions_changed();
  return true;
}

bool NodeService::start_browse(int timeout_ms) {
  if (discovery_busy_.exchange(true)) return false;
  if (discovery_thread_.joinable()) discovery_thread_.join();
  discovery_thread_ = std::thread([this, timeout_ms]() {
    run_browse(timeout_ms);
    discovery_busy_.store(false);
  });
  return true;
}

bool NodeService::scan_lan_peers(int timeout_ms) {
  if (discovery_busy_.exchange(true)) return false;
  if (discovery_thread_.joinable()) discovery_thread_.join();
  discovery_thread_ = std::thread([this, timeout_ms]() {
    run_lan_scan(timeout_ms);
    discovery_busy_.store(false);
  });
  return true;
}

bool NodeService::update_group_meta(const std::string& group_id_hex,
                                    const std::string& description,
                                    const std::string& direction, const std::string& tags,
                                    bool public_listed) {
  nyx::GroupId gid{};
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, gid)) return false;
  const auto profile = load_profile();
  nyx::GroupStore store;
  store.load();
  const auto group = store.find(gid);
  if (!group) return false;
  if (group->owner_id != profile.user_id()) {
    bool owner_member = false;
    for (const auto& m : group->members) {
      if (m.role == nyx::GroupRole::Owner && m.user_id == profile.user_id()) {
        owner_member = true;
        break;
      }
    }
    if (!owner_member) return false;
  }
  const auto visibility = public_listed ? nyx::GroupVisibility::PublicListed
                                        : nyx::GroupVisibility::Circle;
  if (!store.update_meta(gid, description, direction, tags, visibility)) return false;

  // Живой hub — пушим мету участникам в эфире.
  {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
      (void)id;
      if (!session || !session->group_hub) continue;
      if (session->group_hub->group().id != gid) continue;
      session->group_hub->publish_meta(description, direction, tags, visibility);
      break;
    }
  }
  return true;
}

bool NodeService::create_group(const std::string& name) {
  if (name.empty()) return false;
  const auto profile = load_profile();
  nyx::GroupStore store;
  store.load();
  const auto group = store.create(name, profile.user_id(), profile.nickname);
  store.save();

  GroupInfoCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_group_created_;
  }
  if (cb) {
    cb(nyx::GroupStore::group_id_hex(group.id),
       nyx::GroupStore::invite_hex(group.invite_token));
  }
  emit_status("поле создано: " + group.name);
  return true;
}

bool NodeService::delete_group(const std::string& group_id_hex) {
  nyx::GroupId group_id{};
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, group_id)) return false;

  stop_session(make_group_session_id(group_id_hex));

  nyx::GroupStore store;
  store.load();
  if (!store.remove(group_id)) return false;

  std::error_code ec;
  std::filesystem::remove(nyx::MessageStore::path_for_group(group_id), ec);
  emit_status("поле удалено");
  return true;
}

bool NodeService::remove_group_member(const std::string& group_id_hex,
                                      const std::string& user_id_hex) {
  nyx::GroupId group_id{};
  nyx::UserId user_id{};
  std::vector<uint8_t> uid_bytes;
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, group_id) ||
      !nyx::from_hex(user_id_hex, uid_bytes) || uid_bytes.size() != user_id.size()) {
    return false;
  }
  std::memcpy(user_id.data(), uid_bytes.data(), uid_bytes.size());

  nyx::GroupStore store;
  store.load();

  auto session = find_session(make_group_session_id(group_id_hex));
  if (session && session->group_hub) {
    if (!session->group_hub->remove_member(user_id)) return false;
  } else if (!store.remove_member(group_id, user_id)) {
    return false;
  }

  emit_status("участник исключён");
  return true;
}

bool NodeService::start_group_hub(const std::string& group_id_hex) {
  const std::string sid = make_group_session_id(group_id_hex);
  {
    auto existing = find_session(sid);
    if (existing && existing->kind == SessionKind::GroupHub) {
      const auto st = existing->state.load();
      // Не убивать Connecting: register/rendezvous может занять секунды.
      if (st == SessionState::Live || st == SessionState::Connecting) return true;
    }
    if (existing) {
      abandon_session_worker(existing);
      std::lock_guard lock(sessions_mutex_);
      sessions_.erase(sid);
    }
  }

  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(sid);
    session = create_session(sid, SessionKind::GroupHub);
    session->ref_id_hex = group_id_hex;
    // Не перехватывать active у открытого чата (фоновый reconnect).
    if (active_session_id_.empty() || active_session_id_ == sid) active_session_id_ = sid;
  }
  session->worker =
      std::thread([this, session, group_id_hex]() { run_group_hub(session, group_id_hex); });
  emit_sessions_changed();
  return true;
}

bool NodeService::start_group_join(const std::string& invite_hex, bool quiet_ui) {
  nyx::InviteToken token{};
  std::string sid = "group:join:" + invite_hex.substr(0, 12);
  std::string ref_hex;
  if (nyx::GroupStore::invite_from_hex(invite_hex, token)) {
    nyx::GroupStore store;
    store.load();
    for (const auto& g : store.all()) {
      if (g.invite_token != token) continue;
      ref_hex = nyx::GroupStore::group_id_hex(g.id);
      sid = make_group_session_id(ref_hex);
      break;
    }
  }

  {
    auto existing = find_session(sid);
    if (existing && existing->kind == SessionKind::GroupMember) {
      const auto st = existing->state.load();
      if (st == SessionState::Live || st == SessionState::Connecting) return true;
    }
    if (existing) {
      abandon_session_worker(existing);
      std::lock_guard lock(sessions_mutex_);
      sessions_.erase(sid);
    }
  }

  // Старый pending-ключ после прошлых версий.
  if (!ref_hex.empty()) {
    const std::string legacy = "group:join:" + invite_hex.substr(0, 12);
    if (legacy != sid) {
      auto legacy_session = find_session(legacy);
      if (legacy_session) {
        abandon_session_worker(legacy_session);
        std::lock_guard lock(sessions_mutex_);
        sessions_.erase(legacy);
      }
    }
  }

  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(sid);
    session = create_session(sid, SessionKind::GroupMember);
    session->ref_id_hex = ref_hex;
    session->quiet_ui.store(quiet_ui);
    if (active_session_id_.empty() || active_session_id_ == sid) active_session_id_ = sid;
  }
  session->worker =
      std::thread([this, session, invite_hex]() { run_group_join(session, invite_hex); });
  emit_sessions_changed();
  return true;
}

bool NodeService::ensure_session(const std::string& chat_key) {
  if (chat_key.empty()) return false;
  if (is_session_up(chat_key)) {
    set_active_session(chat_key);
    return true;
  }
  intent_store_.load();
  if (!intent_store_.is_enabled(chat_key)) return false;

  if (chat_key.rfind("group:", 0) == 0) {
    const std::string gid = chat_key.substr(6);
    nyx::Profile profile;
    if (!nyx::active_profile(profile)) return false;
    nyx::GroupStore store;
    store.load();
    nyx::GroupId group_id{};
    if (!nyx::GroupStore::group_id_from_hex(gid, group_id)) return false;
    const auto group = store.find(group_id);
    if (!group) return false;
    if (group->owner_id == profile.user_id()) return start_group_hub(gid);
    return start_group_join(nyx::GroupStore::invite_hex(group->invite_token));
  }

  if (chat_key.rfind("dm:", 0) == 0) {
    const std::string peer_hex = chat_key.substr(3);
    nyx::ContactBook book(nyx::default_contacts_path());
    book.load();
    for (const auto& c : book.contacts()) {
      if (nyx::to_hex(c.user_id.data(), c.user_id.size()) != peer_hex) continue;
      if (c.dm_inbox_token_hex.size() == 64) {
        return start_connect_token(c.dm_inbox_token_hex);
      }
    }
    if (const auto* intent = intent_store_.find(chat_key)) {
      if (intent->invite_hex.size() == 64) return start_connect_token(intent->invite_hex);
    }
    return false;
  }
  return false;
}

void NodeService::ensure_owned_hubs_running() {
  nyx::Profile profile;
  if (!nyx::active_profile(profile)) return;

  nyx::GroupStore store;
  store.load();
  intent_store_.load();
  for (const auto& g : store.all()) {
    if (g.owner_id != profile.user_id()) continue;
    const std::string gid = nyx::GroupStore::group_id_hex(g.id);
    const std::string key = make_group_session_id(gid);
    nyx::SessionIntent intent;
    intent.key = key;
    intent.kind = nyx::SessionIntentKind::GroupHub;
    intent.ref_id_hex = gid;
    intent.invite_hex = nyx::GroupStore::invite_hex(g.invite_token);
    intent.enabled = true;
    intent_store_.enable(std::move(intent));
    if (!is_session_up(key)) start_group_hub(gid);
  }
  intent_store_.save();
  start_dm_inbox();
}

void NodeService::auto_reconnect_all() {
  start_dm_inbox();

  nyx::Profile profile;
  if (!nyx::active_profile(profile)) return;

  nyx::GroupStore store;
  store.load();
  intent_store_.load();

  // Свои поля: поднимаем hub всегда, пока intent не выключен вручную («Отключиться»).
  for (const auto& g : store.all()) {
    if (g.owner_id != profile.user_id()) continue;
    const std::string gid = nyx::GroupStore::group_id_hex(g.id);
    const std::string key = make_group_session_id(gid);
    if (!intent_store_.is_enabled(key)) continue;
    if (is_session_up(key)) continue;
    start_group_hub(gid);
  }

  if (!network_config_.auto_start_owned_hub) return;

  // Чужие поля / join — только если intent явно включён (не после «Отключиться»).
  // После 3 видимых неудач — офлайн в UI, тихий probe раз в ~60 с.
  const int64_t now_ms = steady_now_ms();
  for (const auto& g : store.all()) {
    if (g.owner_id == profile.user_id()) continue;
    const std::string gid = nyx::GroupStore::group_id_hex(g.id);
    const std::string key = make_group_session_id(gid);
    if (!intent_store_.is_enabled(key)) continue;
    if (is_session_up(key)) continue;
    const auto* intent = intent_store_.find(key);
    const std::string invite =
        (intent && intent->invite_hex.size() == 64)
            ? intent->invite_hex
            : nyx::GroupStore::invite_hex(g.invite_token);

    bool quiet = false;
    {
      std::lock_guard lock(join_reconnect_mutex_);
      const auto it = join_reconnect_.find(key);
      if (it != join_reconnect_.end() && it->second.failures >= kMaxVisibleJoinRetries) {
        if (now_ms < it->second.next_attempt_ms) continue;
        it->second.next_attempt_ms = now_ms + kQuietJoinProbeIntervalMs;
        quiet = true;
      }
    }
    start_group_join(invite, quiet);
  }

  intent_store_.load();
  for (const auto& intent : intent_store_.all()) {
    if (!intent.enabled) continue;
    if (intent.kind != nyx::SessionIntentKind::Direct) continue;
    if (is_session_up(intent.key)) continue;
    if (intent.invite_hex.size() != 64) continue;

    bool quiet = false;
    {
      std::lock_guard lock(join_reconnect_mutex_);
      auto it = join_reconnect_.find(intent.key);
      if (it == join_reconnect_.end())
        it = join_reconnect_.find("dm:pending:" + intent.invite_hex.substr(0, 8));
      if (it != join_reconnect_.end() && it->second.failures >= kMaxVisibleJoinRetries) {
        if (now_ms < it->second.next_attempt_ms) continue;
        it->second.next_attempt_ms = now_ms + kQuietJoinProbeIntervalMs;
        quiet = true;
      }
    }
    start_connect_token(intent.invite_hex, quiet);
  }
}

std::vector<nyx::GroupRecord> NodeService::list_groups() const {
  nyx::GroupStore store;
  store.load();
  auto groups = store.all();

  auto merge_live = [&](const nyx::GroupRecord& live) {
    for (auto& g : groups) {
      if (g.id != live.id) continue;
      nyx::GroupStore::merge_member_roster(g.members, live.members);
      if (!live.name.empty()) g.name = live.name;
      for (const auto& m : live.members) {
        if (m.role != nyx::GroupRole::Owner) continue;
        g.owner_id = m.user_id;
        break;
      }
      nyx::GroupStore::ensure_roster(g);
      return;
    }
    nyx::GroupRecord copy = live;
    nyx::GroupStore::ensure_roster(copy);
    groups.push_back(std::move(copy));
  };

  {
    std::lock_guard lock(live_group_mutex_);
    for (const auto& [id, live] : live_group_snapshots_) {
      (void)id;
      merge_live(live);
    }
  }

  return groups;
}

void NodeService::set_live_group_snapshot(const nyx::GroupId& id, nyx::GroupRecord rec) {
  std::lock_guard lock(live_group_mutex_);
  live_group_snapshots_[id] = std::move(rec);
}

void NodeService::clear_live_group_snapshot(const nyx::GroupId& id) {
  std::lock_guard lock(live_group_mutex_);
  live_group_snapshots_.erase(id);
}

void NodeService::sync_live_group_from_session(const std::shared_ptr<NetSession>& session) {
  if (!session) return;
  nyx::GroupRecord live;
  if (session->group_hub) {
    live = session->group_hub->group();
  } else if (session->group_member) {
    const auto& view = session->group_member->view();
    live.id = view.id;
    live.name = view.name;
    live.members = view.members;
    if (view.meta_received) {
      live.description = view.description;
      live.direction = view.direction;
      live.tags = view.tags;
      live.visibility = view.visibility;
    }
    nyx::GroupStore store;
    store.load();
    if (const auto stored = store.find(live.id)) {
      live.invite_token = stored->invite_token;
      nyx::UserId zero{};
      if (stored->owner_id != zero) live.owner_id = stored->owner_id;
      nyx::GroupStore::merge_member_roster(live.members, stored->members);
      if (!view.meta_received) {
        live.description = stored->description;
        live.direction = stored->direction;
        live.tags = stored->tags;
        live.visibility = stored->visibility;
      }
    }
    for (const auto& m : view.members) {
      if (m.role == nyx::GroupRole::Owner) {
        live.owner_id = m.user_id;
        break;
      }
    }
  } else {
    return;
  }

  nyx::GroupStore::ensure_roster(live);

  {
    nyx::GroupStore store;
    store.load();
    if (const auto stored = store.find(live.id)) {
      nyx::GroupRecord merged = *stored;
      nyx::GroupStore::merge_member_roster(merged.members, live.members);
      if (!live.name.empty()) merged.name = live.name;
      nyx::UserId zero{};
      if (live.owner_id != zero) merged.owner_id = live.owner_id;
      // Hub всегда источник меты; участник — только после GroupMeta.
      if (session->group_hub ||
          (session->group_member && session->group_member->view().meta_received)) {
        merged.description = live.description;
        merged.direction = live.direction;
        merged.tags = live.tags;
        merged.visibility = live.visibility;
      }
      nyx::GroupStore::ensure_roster(merged);
      store.upsert(merged);
      store.save();
    } else {
      store.upsert(live);
      store.save();
    }
  }

  set_live_group_snapshot(live.id, std::move(live));
}

}  // namespace nyx_app
