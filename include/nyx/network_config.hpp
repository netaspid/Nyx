#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

enum class DiscoveryMode : uint8_t {
  Auto = 0,
  LanOnly = 1,
  Internet = 2,
};

struct RendezvousServer {
  std::string host;
  uint16_t port = 3478;
  std::string label;
};

struct NetworkConfig {
  DiscoveryMode mode = DiscoveryMode::Auto;
  std::vector<RendezvousServer> rendezvous_servers;
  bool use_stun = true;
  std::string stun_host = "stun.l.google.com";
  uint16_t stun_port = 19302;

  uint32_t register_refresh_sec = 120;

  bool auto_start_owned_hub = true;

  static std::string config_path();
  bool load();
  bool save() const;

  RendezvousServer primary_rendezvous() const;

  std::string rendezvous_list_string() const;

  static bool parse_rendezvous_list(const std::string& csv, NetworkConfig& out);
};

} // namespace nyx
