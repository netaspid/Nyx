#pragma once

/** @file mdns.hpp
 *  LAN discovery via the Nyx multicast beacon.
 */

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

/** Extra IPv4 hosts that should receive unicast discovery beacons (e.g. Live DM peers). */
void add_discovery_unicast_target(const std::string& ipv4);
std::vector<std::string> discovery_unicast_targets();

/** Node discovered on the local network. */
struct LanPeer {
  std::string instance;
  std::string host;
  uint16_t port = 0;
  std::string user_id_short;
};

/** Periodic beacon announce and browse. */
class MdnsLan {
public:
  MdnsLan() = default;
  ~MdnsLan();

  MdnsLan(const MdnsLan&) = delete;
  MdnsLan& operator=(const MdnsLan&) = delete;

  /** Prepares the socket for discovery beacons (bind + multicast join). */
  static bool setup_socket(UdpSocket& socket, std::string* err = nullptr);

  /** Background announces every ~3 s. */
  void start_advertising(UdpSocket socket, Profile profile, uint16_t port, std::string host_ip);
  void stop_advertising();

  /** Browses the LAN, collecting Nyx beacon replies. */
  static std::vector<LanPeer> browse(UdpSocket& socket, int timeout_ms = 3000);

  /** One announce (+ optional unicast to peers that miss ethernet multicast). */
  static bool send_announcement(UdpSocket& socket,
                                const Profile& profile,
                                uint16_t port,
                                const std::string& host_ip,
                                const std::vector<std::string>& unicast_hosts = {});

  /** Parses a beacon packet (exposed for tests). */
  static std::optional<LanPeer> parse_beacon(const ByteBuffer& data, const std::string& from_host);

private:
  std::atomic<bool> running_ {false};
  std::thread thread_;
  UdpSocket advert_socket_;
};

} // namespace nyx
