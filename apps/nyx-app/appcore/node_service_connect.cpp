#include "node_service.hpp"

#include "connect_via_hint.hpp"
#include "direct_chat_loop.hpp"

#include "nyx/app.hpp"
#include "nyx/log.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/session_intent.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace nyx_app {

void NodeService::run_dm_inbox(std::shared_ptr<NetSession> session) {
  set_mode(NodeMode::Listening);
  const auto profile = load_profile();

  nyx::InviteToken token{};
  if (!nyx::load_or_create_dm_inbox_token(token)) {
    emit_status("не удалось создать DM inbox token");
    finish_session(session, SessionState::Offline);
    return;
  }

  const std::string token_hex = nyx::to_hex(token.data(), token.size());
  {
    TokenCallback token_cb;
    {
      std::lock_guard lock(cb_mutex_);
      token_cb = on_invite_token_;
    }
    if (token_cb) token_cb(token_hex);
  }

  while (session->running.load()) {
    session->state.store(SessionState::Live);
    emit_sessions_changed();

    std::string rendezvous_host;
    uint16_t rendezvous_port = 0;
    if (network_config_.mode != nyx::DiscoveryMode::LanOnly &&
        !parse_rendezvous(rendezvous_host, rendezvous_port)) {
      emit_status("неверный rendezvous: " + rendezvous_);
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }

    nyx::UdpSocket listen_socket;
    std::string bind_error;
    if (!listen_socket.bind("0.0.0.0", 0, &bind_error)) {
      emit_status("inbox bind failed: " + bind_error);
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }

    nyx::RendezvousPool pool(std::move(listen_socket));
    pool.set_servers(network_config_.rendezvous_servers);
    if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
      if (!pool.register_token(token)) {
        emit_status("inbox register failed");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
    }

    session->mdns = std::make_unique<nyx::MdnsLan>();
    const std::string lan_ip = nyx::guess_lan_ipv4();
    if (network_config_.mode != nyx::DiscoveryMode::Internet) {
      session->mdns->start_advertising(pool.socket(), profile, pool.socket().local_port(),
                                       lan_ip);
    }

    emit_status("DM inbox слушает (token готов)");

    const auto refresh_interval =
        std::chrono::seconds(network_config_.register_refresh_sec);
    auto last_register = std::chrono::steady_clock::now();

    std::string peer_host;
    uint16_t peer_port = 0;
    nyx::ByteBuffer first_packet;
    bool accepted = false;
    while (session->running.load()) {
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
        accepted = true;
        break;
      }
    }

    session->mdns.reset();
    if (!session->running.load()) {
      if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
        pool.unregister_token(token);
      }
      break;
    }
    if (!accepted) continue;

    auto conn = nyx::Connection::accept_responder(std::move(pool.socket()), peer_host,
                                                  peer_port, &first_packet);
    if (!conn) {
      emit_status("inbox handshake timeout");
      continue;
    }

    // Отдельная DM-сессия; inbox перезапустит listen в следующей итерации.
    std::shared_ptr<NetSession> dm;
    {
      std::lock_guard lock(sessions_mutex_);
      const std::string pending = "dm:incoming:" + peer_host + ":" + std::to_string(peer_port);
      dm = create_session(pending, SessionKind::Direct);
      if (active_session_id_.empty()) active_session_id_ = pending;
    }
    auto connection = std::make_unique<nyx::Connection>(std::move(*conn));
    dm->worker = std::thread([this, dm, profile, connection = std::move(connection)]() mutable {
      run_direct_chat(dm, std::move(connection), profile, true, ConnectionVia::Incoming);
    });
    emit_sessions_changed();
  }

  finish_session(session, SessionState::Disconnected);
  emit_status("DM inbox остановлен");
}

void NodeService::run_listen(std::shared_ptr<NetSession> session, bool lan_advertise) {
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
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::UdpSocket listen_socket;
  std::string bind_error;
  if (!listen_socket.bind("0.0.0.0", 0, &bind_error)) {
    emit_status("bind failed: " + bind_error);
    finish_session(session, SessionState::Offline);
    return;
  }

  nyx::RendezvousPool pool(std::move(listen_socket));
  pool.set_servers(network_config_.rendezvous_servers);
  if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
    if (!pool.register_token(token)) {
      emit_status("register failed — проверьте rendezvous (" + rendezvous_list_string() + ")");
      finish_session(session, SessionState::Offline);
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
  session->state.store(SessionState::Live);
  emit_sessions_changed();

  session->mdns = std::make_unique<nyx::MdnsLan>();
  const std::string lan_ip = nyx::guess_lan_ipv4();
  if (network_config_.mode != nyx::DiscoveryMode::Internet && lan_advertise) {
    session->mdns->start_advertising(pool.socket(), profile, pool.socket().local_port(), lan_ip);
    emit_status("LAN: " + lan_ip + ':' + std::to_string(pool.socket().local_port()));
  } else if (!lan_advertise) {
    session->mdns.reset();
  }

  emit_status("ожидание подключения (UDP :" +
              std::to_string(pool.socket().local_port()) + ")...");

  const auto refresh_interval =
      std::chrono::seconds(network_config_.register_refresh_sec);
  auto last_register = std::chrono::steady_clock::now();

  std::string peer_host;
  uint16_t peer_port = 0;
  nyx::ByteBuffer first_packet;
  while (session->running.load()) {
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

  session->mdns.reset();
  if (!session->running.load()) {
    if (network_config_.mode != nyx::DiscoveryMode::LanOnly) {
      pool.unregister_token(token);
    }
    finish_session(session, SessionState::Disconnected);
    return;
  }

  auto conn = nyx::Connection::accept_responder(std::move(pool.socket()), peer_host, peer_port,
                                                &first_packet);
  if (!conn) {
    emit_status("handshake timeout");
    finish_session(session, SessionState::Offline);
    return;
  }

  // Превращаем listen-сессию в Direct на том же id, затем переименуем после Hello.
  session->kind = SessionKind::Direct;
  run_direct_chat(session, std::make_unique<nyx::Connection>(std::move(*conn)), profile, true,
                  ConnectionVia::Incoming);
}

void NodeService::run_connect_token(std::shared_ptr<NetSession> session, std::string token_hex) {
  set_mode(NodeMode::ChatDirect);
  const auto profile = load_profile();
  emit_status("профиль: " + profile.nickname);

  nyx::ByteBuffer token_bytes;
  if (!nyx::from_hex(token_hex, token_bytes) || token_bytes.size() != 32) {
    emit_status("token должен быть 64 hex-символа");
    finish_session(session, SessionState::Offline);
    return;
  }
  nyx::InviteToken token{};
  std::memcpy(token.data(), token_bytes.data(), 32);

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_rendezvous(rendezvous_host, rendezvous_port)) {
    emit_status("неверный rendezvous");
    finish_session(session, SessionState::Offline);
    return;
  }

  emit_status("lookup на rendezvous (" + rendezvous_list_string() + ")...");
  nyx::RendezvousPool pool(std::move(socket));
  pool.set_servers(network_config_.rendezvous_servers);
  auto hint = pool.lookup(token);
  if (!hint) {
    emit_status("lookup failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  emit_status("подключение к " + hint->host_string() + ':' + std::to_string(hint->port) +
              "...");

  auto result = connect_via_rendezvous_hint(pool.socket(), *hint);
  if (!result.connection) {
    emit_status("handshake failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  run_direct_chat(session, std::make_unique<nyx::Connection>(std::move(*result.connection)),
                  profile, false, ConnectionVia::Rendezvous);
}

void NodeService::run_connect_peer(std::shared_ptr<NetSession> session, std::string host,
                                   uint16_t port) {
  const auto profile = load_profile();
  emit_status("подключение к " + host + ':' + std::to_string(port) + "...");

  nyx::UdpSocket socket;
  if (!socket.bind("0.0.0.0", 0)) {
    emit_status("bind failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  auto conn = nyx::Connection::connect_initiator(std::move(socket), host, port);
  if (!conn) {
    emit_status("handshake failed");
    finish_session(session, SessionState::Offline);
    return;
  }

  run_direct_chat(session, std::make_unique<nyx::Connection>(std::move(*conn)), profile, false,
                  ConnectionVia::LanDirect);
}

void NodeService::run_browse(int timeout_ms) {
  nyx::UdpSocket socket;
  std::string err;
  if (!nyx::MdnsLan::setup_socket(socket, &err)) {
    emit_status("multicast: " + err);
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

bool NodeService::start_listen(bool lan_advertise) {
  const std::string sid = "listen:ephemeral";
  std::shared_ptr<NetSession> existing;
  {
    std::lock_guard lock(sessions_mutex_);
    existing = find_session_locked(sid);
    if (existing) existing->running.store(false);
  }
  if (existing && existing->worker.joinable() &&
      existing->worker.get_id() != std::this_thread::get_id()) {
    existing->worker.join();
  }

  std::shared_ptr<NetSession> session;
  {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(sid);
    session = create_session(sid, SessionKind::DmInbox);
  }
  session->worker =
      std::thread([this, session, lan_advertise]() { run_listen(session, lan_advertise); });
  set_mode(NodeMode::Listening);
  emit_sessions_changed();
  return true;
}

}  // namespace nyx_app
