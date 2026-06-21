#include "group_session.hpp"

#include "cli_console.hpp"
#include "connect_via_hint.hpp"

#include "nyx/app.hpp"
#include "nyx/connection.hpp"
#include "nyx/group.hpp"
#include "nyx/group_hub.hpp"
#include "nyx/group_member.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace nyx_node {

namespace {

nyx::Profile load_profile(const NodeConfig& config) {
  const std::string path =
      config.profile_path.empty() ? nyx::default_profile_path() : config.profile_path;
  return nyx::load_or_create_profile(path, config.nickname);
}

}  // namespace

int run_group_create(const std::string& name, const NodeConfig& config) {
  if (name.empty()) {
    std::cerr << "укажите имя поля: group create <имя>\n";
    return 1;
  }
  const auto profile = load_profile(config);
  nyx::GroupStore store;
  const auto group = store.create(name, profile.user_id(), profile.nickname);

  std::cout << "поле создано: " << group.name << '\n';
  std::cout << "  group_id: " << nyx::GroupStore::group_id_hex(group.id) << '\n';
  std::cout << "  invite:   " << nyx::GroupStore::invite_hex(group.invite_token) << '\n';
  std::cout << "\nЗапуск hub: nyx-node group hub --group "
            << nyx::GroupStore::group_id_hex(group.id) << '\n';
  return 0;
}

int run_group_hub(const std::string& group_id_hex, const NodeConfig& config) {
  nyx::GroupId group_id{};
  if (!nyx::GroupStore::group_id_from_hex(group_id_hex, group_id)) {
    std::cerr << "неверный group_id (64 hex)\n";
    return 1;
  }

  nyx::GroupStore store;
  auto group = store.find(group_id);
  if (!group) {
    std::cerr << "поле не найдено в " << nyx::GroupStore::store_path() << '\n';
    return 1;
  }

  const auto profile = load_profile(config);
  if (group->owner_id != profile.user_id()) {
    std::cerr << "только владелец поля может запустить hub\n";
    return 1;
  }

  CliConsole ui(profile.nickname);
  ui.print_event("hub поля «" + group->name + "»");

  nyx::UdpSocket socket;
  std::string bind_error;
  if (!socket.bind(config.bind_host, config.bind_port, &bind_error)) {
    std::cerr << "bind: " << bind_error << '\n';
    return 1;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_host_port(config.rendezvous, rendezvous_host, rendezvous_port)) {
    std::cerr << "неверный rendezvous\n";
    return 1;
  }

  nyx::RendezvousClient rv(std::move(socket), rendezvous_host, rendezvous_port);
  if (!rv.register_token(group->invite_token)) {
    std::cerr << "не удалось зарегистрировать invite на rendezvous\n";
    return 1;
  }

  ui.print_event("group invite: " + nyx::GroupStore::invite_hex(group->invite_token));
  ui.print_event("ожидание участников (UDP :" + std::to_string(rv.socket().local_port()) +
                 ")...");

  nyx::GroupHub hub(rv.socket(), profile, *group);
  hub.set_on_message([&](const nyx::ChatMessage& msg, bool outgoing) {
    ui.print_message(msg.timestamp_ms, msg.author, msg.text, outgoing);
  });
  hub.set_on_event([&](const std::string& text) { ui.print_event(text); });

  ui.print_header(group->name, "поле · hub");
  ui.print_event("команды: /members /help /quit");

  std::atomic<bool> running{true};
  std::thread net([&] {
    while (running.load()) {
      hub.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  ui.print_prompt();
  std::string line;
  while (running.load() && std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line == "/help") {
      ui.print_help();
      ui.print_event("/members — список участников");
      ui.print_prompt();
      continue;
    }
    if (line == "/members") {
      ui.print_event("участники (" + std::to_string(hub.group().members.size()) + "):");
      for (const auto& m : hub.group().members) {
        ui.print_event("  " + m.nickname + " (" + nyx::short_user_id(m.user_id) + ")" +
                       (m.role == nyx::GroupRole::Owner ? " owner" : ""));
      }
      ui.print_prompt();
      continue;
    }
    if (line.empty()) {
      ui.print_prompt();
      continue;
    }
    if (line.rfind('/', 0) == 0) {
      ui.print_event("неизвестная команда: " + line);
      ui.print_prompt();
      continue;
    }
    hub.send_message(line);
    ui.print_prompt();
  }

  running.store(false);
  if (net.joinable()) net.join();
  ui.print_event("hub остановлен");
  return 0;
}

int run_group_join(const std::string& token_hex, const NodeConfig& config) {
  const auto profile = load_profile(config);
  CliConsole ui(profile.nickname);

  nyx::InviteToken token{};
  if (!nyx::GroupStore::invite_from_hex(token_hex, token)) {
    std::cerr << "invite token: 64 hex символа\n";
    return 1;
  }

  nyx::UdpSocket socket;
  std::string bind_error;
  if (!socket.bind(config.bind_host, config.bind_port, &bind_error)) {
    std::cerr << "bind: " << bind_error << '\n';
    return 1;
  }

  std::string rendezvous_host;
  uint16_t rendezvous_port = 0;
  if (!parse_host_port(config.rendezvous, rendezvous_host, rendezvous_port)) {
    std::cerr << "неверный rendezvous\n";
    return 1;
  }

  ui.print_event("поиск hub поля...");
  nyx::RendezvousClient rv(std::move(socket), rendezvous_host, rendezvous_port);
  auto hint = rv.lookup(token);
  if (!hint) {
    std::cerr << "hub не найден (token / rendezvous)\n";
    return 1;
  }

  auto result = nyx_app::connect_via_rendezvous_hint(rv.socket(), *hint);
  if (!result.connection) {
    std::cerr << "handshake не завершён\n";
    return 1;
  }

  auto connection = std::move(*result.connection);
  connection.ping();
  ui.print_event("соединение с hub " + result.host);

  nyx::HelloMessage hub_hello;
  if (!nyx::exchange_hello(connection, profile, hub_hello)) {
    std::cerr << "обмен Hello не удался\n";
    return 1;
  }
  ui.print_event("hub: " + hub_hello.nickname);

  nyx::GroupId zero{};
  nyx::GroupMemberService member(connection, profile, zero, "");
  member.set_on_message([&](const nyx::ChatMessage& msg, bool outgoing) {
    ui.print_message(msg.timestamp_ms, msg.author, msg.text, outgoing);
  });
  member.set_on_event([&](const std::string& text) { ui.print_event(text); });

  if (!member.join()) {
    std::cerr << "не удалось войти в поле\n";
    return 1;
  }

  ui.print_header(member.view().name, "поле");
  ui.print_event("/members /help /quit");

  std::atomic<bool> running{true};
  std::thread net([&] {
    while (running.load() && member.joined()) {
      member.tick();
      nyx::ByteBuffer payload;
      uint32_t stream_id = 0;
      while (connection.recv_stream(stream_id, payload)) {
        if (stream_id == nyx::kChatStream) member.handle_payload(payload);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  ui.print_prompt();
  std::string line;
  while (running.load() && member.joined() && std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line == "/help") {
      ui.print_help();
      ui.print_prompt();
      continue;
    }
    if (line == "/members") {
      for (const auto& m : member.view().members) {
        ui.print_event("  " + m.nickname + " (" + nyx::short_user_id(m.user_id) + ")");
      }
      ui.print_prompt();
      continue;
    }
    if (line.empty()) {
      ui.print_prompt();
      continue;
    }
    if (line.rfind('/', 0) == 0) {
      ui.print_event("неизвестная команда");
      ui.print_prompt();
      continue;
    }
    if (!member.send_message(line)) {
      ui.print_event("ошибка отправки");
      break;
    }
    ui.print_prompt();
  }

  running.store(false);
  if (net.joinable()) net.join();
  return 0;
}

}  // namespace nyx_node
