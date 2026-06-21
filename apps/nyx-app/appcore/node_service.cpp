#include "node_service.hpp"

#include "nyx/log.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/util.hpp"

namespace nyx_app {

NodeService::NodeService() {
  nyx::log_init();
  load_network_config();
}

NodeService::~NodeService() { stop(); }

void NodeService::set_profile_path(std::string path) { profile_path_ = std::move(path); }
void NodeService::set_nickname(std::string nickname) { nickname_ = std::move(nickname); }

void NodeService::set_rendezvous(std::string addr) {
  rendezvous_ = std::move(addr);
  nyx::NetworkConfig tmp;
  if (nyx::NetworkConfig::parse_rendezvous_list(rendezvous_, tmp)) {
    network_config_.rendezvous_servers = tmp.rendezvous_servers;
  }
}

void NodeService::set_rendezvous_list(const std::string& csv) {
  nyx::NetworkConfig tmp;
  if (!nyx::NetworkConfig::parse_rendezvous_list(csv, tmp)) return;
  network_config_.rendezvous_servers = tmp.rendezvous_servers;
  rendezvous_ = network_config_.rendezvous_list_string();
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

nyx::Profile NodeService::profile() const { return load_profile(); }

nyx::Profile NodeService::load_profile() const {
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

void NodeService::emit_chat_ready(const std::string& peer_title, ConnectionVia via,
                                  const std::string& peer_host) {
  ChatReadyCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_chat_ready_;
  }
  if (cb) cb(peer_title, nyx_app::connection_label(via, peer_host));
}

void NodeService::set_mode(NodeMode mode) { mode_.store(mode); }

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
  set_mode(NodeMode::Idle);
  busy_.store(false);
}

bool NodeService::start_listen(bool lan_advertise) {
  if (busy_.load()) return false;
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, lan_advertise]() { run_listen(lan_advertise); });
  return true;
}

bool NodeService::start_connect_token(const std::string& token_hex) {
  if (busy_.load()) return false;
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, token_hex]() { run_connect_token(token_hex); });
  return true;
}

bool NodeService::start_connect_peer(const std::string& host, uint16_t port) {
  if (busy_.load()) return false;
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, host, port]() { run_connect_peer(host, port); });
  return true;
}

bool NodeService::start_browse(int timeout_ms) {
  if (busy_.load()) return false;
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

bool NodeService::start_group_hub(const std::string& group_id_hex) {
  if (busy_.load()) return false;
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, group_id_hex]() { run_group_hub(group_id_hex); });
  return true;
}

bool NodeService::start_group_join(const std::string& invite_hex) {
  if (busy_.load()) return false;
  stop();
  running_.store(true);
  busy_.store(true);
  worker_ = std::thread([this, invite_hex]() { run_group_join(invite_hex); });
  return true;
}

std::vector<nyx::GroupRecord> NodeService::list_groups() const {
  nyx::GroupStore store;
  store.load();
  return store.all();
}

}  // namespace nyx_app
