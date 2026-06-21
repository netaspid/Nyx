#pragma once

/** @file network_config.hpp
 *  Настройки обнаружения: список rendezvous, режим LAN/Интернет.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

/** Режим подключения через bootstrap. */
enum class DiscoveryMode : uint8_t {
  Auto = 0,     /**< LAN + rendezvous */
  LanOnly = 1,  /**< только mDNS / прямой peer */
  Internet = 2, /**< rendezvous + hole punch (без LAN-only) */
};

/** Один bootstrap-сервер (host:port). */
struct RendezvousServer {
  std::string host;
  uint16_t port = 3478;
  std::string label; /**< «VDS EU», для UI */
};

/** Сохраняемая конфигурация сети: data_dir()/network.json. */
struct NetworkConfig {
  DiscoveryMode mode = DiscoveryMode::Auto;
  std::vector<RendezvousServer> rendezvous_servers;
  bool use_stun = true;
  std::string stun_host = "stun.l.google.com";
  uint16_t stun_port = 19302;
  /** Интервал повторной Register на rendezvous (сек). */
  uint32_t register_refresh_sec = 120;

  static std::string config_path();
  bool load();
  bool save() const;

  /** Первый сервер или localhost:3478. */
  RendezvousServer primary_rendezvous() const;

  /** Строка host:port,host:port для CLI. */
  std::string rendezvous_list_string() const;

  /** Парсит список из CSV host:port. */
  static bool parse_rendezvous_list(const std::string& csv, NetworkConfig& out);
};

}  // namespace nyx
