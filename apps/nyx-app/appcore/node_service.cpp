#include "node_service.hpp"

#include "nyx/account_store.hpp"
#include "nyx/log.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/message_store.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "nyx/util.hpp"

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
}

void NodeService::clear_account_data() {
  file_index_.clear();
  file_access_.clear();
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
    if (c == '\\' && i < json.size()) out.push_back(json[i++]);
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

void NodeService::emit_message(const nyx::ChatMessage& msg, bool outgoing) {
  MessageCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_message_;
  }
  if (!cb) return;
  UiMessage ui;
  ui.timestamp_ms = msg.timestamp_ms;
  ui.author = msg.author;
  ui.text = msg.text;
  ui.outgoing = outgoing;
  cb(ui);
}

void NodeService::emit_session_ended() {
  SessionEndedCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_session_ended_;
  }
  if (cb) cb();
}

void NodeService::emit_chat_ready(const std::string& title, ConnectionVia via,
                                  const std::string& peer_host, nyx::ConversationKind kind,
                                  const std::string& ref_id_hex) {
  ChatReadyCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_chat_ready_;
  }
  if (cb) cb(title, nyx_app::connection_label(via, peer_host), kind, ref_id_hex);
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

bool NodeService::session_blocks_new_work() const {
  const auto m = mode_.load();
  return m == NodeMode::ChatDirect || m == NodeMode::GroupHub || m == NodeMode::GroupMember;
}

void NodeService::stop() {
  running_.store(false);
  if (discovery_thread_.joinable()) discovery_thread_.join();
  if (worker_.joinable()) worker_.join();
  mdns_.reset();
  chat_.reset();
  files_.reset();
  group_hub_.reset();
  group_member_.reset();
  connection_.reset();
  share_scope_group_ = {};
  clear_live_group_snapshot();
  set_mode(NodeMode::Idle);
  busy_.store(false);
}

bool NodeService::start_listen(bool lan_advertise) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, lan_advertise]() { run_listen(lan_advertise); });
  return true;
}

bool NodeService::start_connect_token(const std::string& token_hex) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, token_hex]() { run_connect_token(token_hex); });
  return true;
}

bool NodeService::start_connect_peer(const std::string& host, uint16_t port) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, host, port]() { run_connect_peer(host, port); });
  return true;
}

bool NodeService::start_browse(int timeout_ms) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, timeout_ms]() { run_browse(timeout_ms); });
  return true;
}

bool NodeService::scan_lan_peers(int timeout_ms) {
  const auto mode = mode_.load();
  if (mode == NodeMode::ChatDirect || mode == NodeMode::GroupHub ||
      mode == NodeMode::GroupMember) {
    return false;
  }
  if (discovery_busy_.exchange(true)) return false;
  if (discovery_thread_.joinable()) discovery_thread_.join();
  discovery_thread_ = std::thread([this, timeout_ms]() {
    run_lan_scan(timeout_ms);
    discovery_busy_.store(false);
  });
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

  if (group_hub_ && share_scope_group_ == group_id) {
    if (!group_hub_->remove_member(user_id)) return false;
  } else if (!store.remove_member(group_id, user_id)) {
    return false;
  }

  emit_status("участник исключён");
  return true;
}

void NodeService::set_auto_start_owned_hub(bool enabled) {
  network_config_.auto_start_owned_hub = enabled;
  save_network_config();
}

std::string NodeService::running_group_hub_id_hex() const {
  if (mode_.load() != NodeMode::GroupHub) return {};
  return nyx::GroupStore::group_id_hex(share_scope_group_);
}

bool NodeService::start_group_hub(const std::string& group_id_hex) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, group_id_hex]() { run_group_hub(group_id_hex); });
  return true;
}

bool NodeService::start_group_join(const std::string& invite_hex) {
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, invite_hex]() { run_group_join(invite_hex); });
  return true;
}

std::vector<nyx::GroupRecord> NodeService::list_groups() const {
  nyx::GroupStore store;
  store.load();
  auto groups = store.all();

  /** Подмешивает снимок roster: объединяет с диском, не затирая ушедших участников. */
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
    if (live_group_snapshot_) merge_live(*live_group_snapshot_);
  }

  return groups;
}

void NodeService::set_live_group_snapshot(nyx::GroupRecord rec) {
  std::lock_guard lock(live_group_mutex_);
  live_group_snapshot_ = std::move(rec);
}

void NodeService::clear_live_group_snapshot() {
  std::lock_guard lock(live_group_mutex_);
  live_group_snapshot_.reset();
}

void NodeService::sync_live_group_from_session() {
  nyx::GroupRecord live;
  if (group_hub_) {
    live = group_hub_->group();
  } else if (group_member_) {
    const auto& view = group_member_->view();
    live.id = view.id;
    live.name = view.name;
    live.members = view.members;
    nyx::GroupStore store;
    store.load();
    if (const auto stored = store.find(live.id)) {
      live.invite_token = stored->invite_token;
      nyx::UserId zero{};
      if (stored->owner_id != zero) live.owner_id = stored->owner_id;
      nyx::GroupStore::merge_member_roster(live.members, stored->members);
    }
    for (const auto& m : view.members) {
      if (m.role == nyx::GroupRole::Owner) {
        live.owner_id = m.user_id;
        break;
      }
    }
  } else {
    clear_live_group_snapshot();
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
      nyx::GroupStore::ensure_roster(merged);
      store.upsert(merged);
    } else {
      store.upsert(live);
    }
  }

  set_live_group_snapshot(std::move(live));
}

}  // namespace nyx_app
