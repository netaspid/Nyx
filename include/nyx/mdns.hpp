#pragma once

#include "nyx/identity.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace nyx {

void add_discovery_unicast_target(const std::string& ipv4);
std::vector<std::string> discovery_unicast_targets();

struct LanPeer {
  std::string instance;
  std::string host;
  uint16_t port = 0;
  std::string user_id_short;
};

class MdnsLan {
public:
  MdnsLan() = default;
  ~MdnsLan();

  MdnsLan(const MdnsLan&) = delete;
  MdnsLan& operator=(const MdnsLan&) = delete;

  static bool setup_socket(UdpSocket& socket, std::string* err = nullptr);

  void start_advertising(UdpSocket socket, Profile profile, uint16_t port, std::string host_ip);
  void stop_advertising();

  static std::vector<LanPeer> browse(UdpSocket& socket, int timeout_ms = 3000);

  static bool send_announcement(UdpSocket& socket,
                                const Profile& profile,
                                uint16_t port,
                                const std::string& host_ip,
                                const std::vector<std::string>& unicast_hosts = {});

  static std::optional<LanPeer> parse_beacon(const ByteBuffer& data, const std::string& from_host);

private:
  std::atomic<bool> running_ {false};
  std::thread thread_;
  UdpSocket advert_socket_;
};

} // namespace nyx
