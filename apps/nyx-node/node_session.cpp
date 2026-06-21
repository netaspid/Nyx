#include "node_session.hpp"

#include "chat_session.hpp"
#include "cli_console.hpp"
#include "connect_via_hint.hpp"

#include "nyx/identity.hpp"
#include "nyx/mdns.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/network_config.hpp"
#include "nyx/rendezvous_pool.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

namespace nyx_node {

namespace {

nyx::Profile load_profile_for_session(const NodeConfig& config) {
  const std::string path =
      config.profile_path.empty() ? nyx::default_profile_path() : config.profile_path;
  return nyx::load_or_create_profile(path, config.nickname);
}

}  // namespace

int run_listen(const NodeConfig& config) {
  const auto profile = load_profile_for_session(config);
  CliConsole ui(profile.nickname);
  ui.print_event("профиль: " + profile.nickname + " (id: " +
                 nyx::short_user_id(profile.user_id()) + ")");

  nyx::InviteToken token{};
  nyx::random_bytes(token.data(), token.size());

  nyx::UdpSocket socket;
  std::string bind_error;
  if (!socket.bind(config.bind_host, config.bind_port, &bind_error)) {
    std::cerr << "не удалось привязать UDP-сокет (" << config.bind_host << ':'
              << config.bind_port << "): " << bind_error << '\n';
    return 1;
  }

  nyx::NetworkConfig net_cfg;
  if (!nyx::NetworkConfig::parse_rendezvous_list(config.rendezvous, net_cfg)) {
    std::cerr << "неверный адрес rendezvous: " << config.rendezvous << '\n';
    return 1;
  }

  nyx::RendezvousPool pool(std::move(socket));
  pool.set_servers(net_cfg.rendezvous_servers);
  if (!pool.register_token(token)) {
    std::cerr << "не удалось зарегистрировать token на rendezvous (" << config.rendezvous
              << ")\nубедитесь, что nyx-rendezvous запущен\n";
    return 1;
  }

  nyx::MdnsLan mdns;
  if (config.enable_lan) {
    const std::string lan_ip = nyx::guess_lan_ipv4();
    mdns.start_advertising(pool.socket(), profile, pool.socket().local_port(), lan_ip);
    ui.print_event("LAN: " + profile.nickname + " @" + lan_ip + ':' +
                   std::to_string(pool.socket().local_port()));
  }

  ui.print_event("invite token: " + nyx::to_hex(token.data(), token.size()));
  ui.print_event("ожидание подключения (UDP :" +
                 std::to_string(pool.socket().local_port()) + ")...");

  const auto refresh = std::chrono::seconds(net_cfg.register_refresh_sec);
  auto last_register = std::chrono::steady_clock::now();

  std::string peer_host;
  uint16_t peer_port = 0;
  nyx::ByteBuffer first_packet;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_register >= refresh) {
      pool.register_token(token);
      last_register = now;
    }
    auto packet = pool.socket().recv_from(peer_host, peer_port, 1000);
    if (!packet) continue;
    if (nyx::is_punch_datagram(*packet)) continue;
    if (nyx::is_handshake_datagram(*packet)) {
      first_packet = std::move(*packet);
      break;
    }
  }

  auto connection = nyx::Connection::accept_responder(
      std::move(pool.socket()), peer_host, peer_port, &first_packet);
  if (!connection) {
    ui.print_event("handshake не завершён (таймаут или ошибка Noise)");
    return 1;
  }

  connection->ping();
  run_chat_session(*connection, profile, true);
  return 0;
}

int run_connect(const std::string& token_hex, const NodeConfig& config) {
  const auto profile = load_profile_for_session(config);
  CliConsole ui(profile.nickname);
  ui.print_event("профиль: " + profile.nickname + " (id: " +
                 nyx::short_user_id(profile.user_id()) + ")");

  nyx::ByteBuffer token_bytes;
  if (!nyx::from_hex(token_hex, token_bytes) || token_bytes.size() != 32) {
    std::cerr << "token должен быть 64 hex-символа (32 байта)\n";
    return 1;
  }
  nyx::InviteToken token{};
  std::memcpy(token.data(), token_bytes.data(), 32);

  nyx::UdpSocket socket;
  std::string bind_error;
  if (!socket.bind(config.bind_host, config.bind_port, &bind_error)) {
    std::cerr << "не удалось привязать UDP-сокет: " << bind_error << '\n';
    return 1;
  }

  nyx::NetworkConfig net_cfg;
  if (!nyx::NetworkConfig::parse_rendezvous_list(config.rendezvous, net_cfg)) {
    std::cerr << "неверный адрес rendezvous: " << config.rendezvous << '\n';
    return 1;
  }

  ui.print_event("поиск узла на rendezvous (" + config.rendezvous + ")...");
  nyx::RendezvousPool pool(std::move(socket));
  pool.set_servers(net_cfg.rendezvous_servers);
  auto hint = pool.lookup(token);
  if (!hint) {
    ui.print_event("lookup не удался: token не найден или rendezvous не отвечает");
    std::cerr << "проверьте token и что listen уже зарегистрировался\n";
    return 1;
  }

  const std::string peer_host = hint->host_string();

  ui.print_event("найден узел " + peer_host + ':' + std::to_string(hint->port) +
                 ", подключение...");

  auto result = nyx_app::connect_via_rendezvous_hint(pool.socket(), *hint);
  if (!result.connection) {
    ui.print_event("handshake не завершён с " + peer_host + ':' +
                   std::to_string(hint->port));
    return 1;
  }

  result.connection->ping();
  run_chat_session(*result.connection, profile, false);
  return 0;
}

int run_connect_peer(const NodeConfig& config) {
  if (config.peer_addr.empty()) {
    std::cerr << "укажите --peer host:port\n";
    return 1;
  }

  std::string peer_host;
  uint16_t peer_port = 0;
  if (!parse_host_port(config.peer_addr, peer_host, peer_port)) {
    std::cerr << "неверный --peer, нужен host:port\n";
    return 1;
  }

  const auto profile = load_profile_for_session(config);
  CliConsole ui(profile.nickname);
  ui.print_event("профиль: " + profile.nickname + " (id: " +
                 nyx::short_user_id(profile.user_id()) + ")");
  ui.print_event("подключение к " + peer_host + ':' + std::to_string(peer_port) + "...");

  nyx::UdpSocket socket;
  std::string bind_error;
  if (!socket.bind(config.bind_host, config.bind_port, &bind_error)) {
    std::cerr << "не удалось привязать UDP-сокет: " << bind_error << '\n';
    return 1;
  }

  auto connection =
      nyx::Connection::connect_initiator(std::move(socket), peer_host, peer_port);
  if (!connection) {
    ui.print_event("handshake не завершён с " + peer_host + ':' + std::to_string(peer_port));
    return 1;
  }

  connection->ping();
  run_chat_session(*connection, profile, false);
  return 0;
}

int run_browse(int timeout_ms) {
  nyx::UdpSocket socket;
  std::string bind_error;
  if (!nyx::MdnsLan::setup_socket(socket, &bind_error)) {
    std::cerr << "multicast: " << bind_error << '\n';
    return 1;
  }

  std::cout << "поиск узлов Nyx в LAN (" << timeout_ms << " ms)...\n";
  const auto peers = nyx::MdnsLan::browse(socket, timeout_ms);
  if (peers.empty()) {
    std::cout << "узлы не найдены (запустите listen на другой машине)\n";
    return 0;
  }

  for (const auto& peer : peers) {
    std::cout << "  " << peer.instance << "  id:" << peer.user_id_short << "  "
              << peer.host << ':' << peer.port << '\n';
  }
  std::cout << "\nПодключение: nyx-node connect --peer " << peers.front().host << ':'
            << peers.front().port << '\n';
  return 0;
}

}  // namespace nyx_node
