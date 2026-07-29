#pragma once

/** @file network_config.hpp
 *  Discovery settings: rendezvous list, LAN/Internet mode.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

/** Bootstrap connection mode. */
enum class DiscoveryMode : uint8_t {
  Auto = 0,     /**< LAN + rendezvous */
  LanOnly = 1,  /**< mDNS / direct peer only */
  Internet = 2, /**< rendezvous + hole punch */
};

/** One bootstrap server (host:port). */
struct RendezvousServer {
  std::string host;
  uint16_t port = 3478;
  std::string label; /**< UI label, e.g. "VDS EU" */
};

/** Persisted network configuration: data_dir()/network.json. */
struct NetworkConfig {
  DiscoveryMode mode = DiscoveryMode::Auto;
  std::vector<RendezvousServer> rendezvous_servers;
  bool use_stun = true;
  std::string stun_host = "stun.l.google.com";
  uint16_t stun_port = 19302;
  /** Re-register interval on rendezvous, seconds. */
  uint32_t register_refresh_sec = 120;
  /** Master switch for hub autostart / session reconnect after launch. */
  bool auto_start_owned_hub = true;

  static std::string config_path();
  bool load();
  bool save() const;

  /** First server, or localhost:3478. */
  RendezvousServer primary_rendezvous() const;

  /** host:port,host:port string for the CLI. */
  std::string rendezvous_list_string() const;

  /** Parses the list from CSV host:port. */
  static bool parse_rendezvous_list(const std::string& csv, NetworkConfig& out);
};

} // namespace nyx
