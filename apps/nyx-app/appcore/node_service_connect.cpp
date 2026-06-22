#include "node_service.hpp"

#include "connect_via_hint.hpp"
#include "direct_chat_loop.hpp"

#include "nyx/app.hpp"
#include "nyx/chat_service.hpp"
#include "nyx/log.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace nyx_app {

void NodeService::run_listen(bool lan_advertise) {
  set_mode(NodeMode::Listening);
  const auto profile = load_profile();
  emit_status("профиль: " + profile.nickname + " (id: " +
              nyx::short_user_id(profile.user_id()) + ")");

  nyx::InviteToken token{};
  nyx::random_bytes(token.data(), token.size());

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (network_config_.mode == nyx::DiscoveryMode::LanOnly) {
    emit_status("режим LAN-only: rendezvous не используется для listen");
  } else if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous: " + rendezvous_);
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::UdpSocket listen_socket;
  std::string bind_error;
  if (!listen_socket.bind("0.0.0.0", 0, &bind_error)) {
    emit_status("bind failed: " + bind_error);
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  nyx::RendezvousPool pool(std::move(listen_socket));
  pool.set_servers(network_config_.rendezvous_servers);
  if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
    if (!pool.register_token(token)) {
      emit_status("register failed — проверьте rendezvous (" + rendezvous_list_string() + ")");
      busy_.store(false);
      set_mode(NodeMode::Idle);
      return;
    }
    emit_status("rendezvous: " + rendezvous_list_string());
  }

  const std::string token_hex = nyx::to_hex(token.data(), token.size());
  TokenCallback token_cb;
  {
    std::lock_guard lock(cb_mutex_);
    token_cb = on_invite_token_;
  }
  if (token_cb) token_cb(token_hex);
  emit_status("invite token: " + token_hex);

  mdns_ = std::make_unique<nyx::MdnsLan>();
  const std::string lan_ip = nyx::guess_lan_ipv4();
  if (network_config_.mode != nyx::DiscoveryMode::Internet && lan_advertise) {
    mdns_->start_advertising(pool.socket(), profile, pool.socket().local_port(), lan_ip);
    emit_status("LAN: " + lan_ip + ':' + std::to_string(pool.socket().local_port()));
  } else if (!lan_advertise) {
    mdns_.reset();
  }

  emit_status("ожидание подключения (UDP :" +
              std::to_string(pool.socket().local_port()) + ")...");

  const auto refresh_interval =
      std::chrono::seconds(network_config_.register_refresh_sec);
  auto last_register = std::chrono::steady_clock::now();

  std::string peer_host;
  uint16_t peer_port = 0;
  nyx::ByteBuffer first_packet;
  while (running_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (network_config_.mode != nyx::DiscoveryMode::LanOnly &&
        now - last_register >= refresh_interval) {
      pool.register_token(token);
      last_register = now;
    }
    auto packet = pool.socket().recv_from(peer_host, peer_port, 500);
    if (!packet) continue;
    if (nyx::is_punch_datagram(*packet)) continue;
    if (nyx::is_handshake_datagram(*packet)) {
      first_packet = std::move(*packet);
      break;
    }
  }

  if (!running_.load()) {
    if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
      pool.unregister_token(token);
    }
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  auto conn = nyx::Connection::accept_responder(
      std::move(pool.socket()), peer_host, peer_port, &first_packet);
  if (!conn) {
    emit_status("handshake timeout");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  run_direct_chat(std::make_unique<nyx::Connection>(std::move(*conn)), profile, true,
                  ConnectionVia::Incoming);
}

void NodeService::run_connect_token(std::string token_hex) {
  set_mode(NodeMode::Listening);
  const auto profile = load_profile();
  emit_status("профиль: " + profile.nickname);

  nyx::ByteBuffer token_bytes;
  if (!nyx::from_hex(token_hex, token_bytes) || token_bytes.size() != 32) {
    emit_status("token должен быть 64 hex-символа");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }
  nyx::InviteToken token{};
  std::memcpy(token.data(), token_bytes.data(), 32);

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  emit_status("lookup на rendezvous (" + rendezvous_list_string() + ")...");
  nyx::RendezvousPool pool(std::move(socket));
  pool.set_servers(network_config_.rendezvous_servers);
  auto hint = pool.lookup(token);
  if (!hint) {
    emit_status("lookup failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  emit_status("подключение к " + hint->host_string() + ':' + std::to_string(hint->port) +
              "...");

  auto result = connect_via_rendezvous_hint(pool.socket(), *hint);
  if (!result.connection) {
    emit_status("handshake failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  run_direct_chat(std::make_unique<nyx::Connection>(std::move(*result.connection)), profile,
                  false, ConnectionVia::Rendezvous);
}

void NodeService::run_connect_peer(std::string host, uint16_t port) {
  const auto profile = load_profile();
  emit_status("подключение к " + host + ':' + std::to_string(port) + "...");

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    busy_.store(false);
    return;
  }

  auto conn = nyx::Connection::connect_initiator(std::move(socket), host, port);
  if (!conn) {
    emit_status("handshake failed");
    busy_.store(false);
    set_mode(NodeMode::Idle);
    return;
  }

  run_direct_chat(std::make_unique<nyx::Connection>(std::move(*conn)), profile, false,
                  ConnectionVia::LanDirect);
}

void NodeService::run_browse(int timeout_ms) {
  nyx::UdpSocket socket;
  std::string err;
  if (!nyx::MdnsLan::setup_socket(socket, &err)) {
    emit_status("multicast: " + err);
    busy_.store(false);
    return;
  }

  emit_status("поиск узлов в LAN...");
  const auto peers = nyx::MdnsLan::browse(socket, timeout_ms);

  std::vector<nyx::LanPeer> out;
  out.reserve(peers.size());
  for (const auto& p : peers) {
    out.push_back(p);
  }

  LanPeersCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_lan_peers_;
  }
  if (cb) cb(out);

  if (out.empty()) {
    emit_status("узлы не найдены (запустите «Слушать» на другой машине)");
  } else {
    emit_status("найдено узлов: " + std::to_string(out.size()));
  }

  busy_.store(false);
  set_mode(NodeMode::Idle);
}

void NodeService::run_lan_scan(int timeout_ms) {
  nyx::UdpSocket socket;
  std::string err;
  if (!nyx::MdnsLan::setup_socket(socket, &err)) return;

  const auto peers = nyx::MdnsLan::browse(socket, timeout_ms);

  std::vector<nyx::LanPeer> out;
  out.reserve(peers.size());
  for (const auto& p : peers) {
    out.push_back(p);
  }

  LanPeersCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_lan_peers_;
  }
  if (cb) cb(out);
}

}  // namespace nyx_app
