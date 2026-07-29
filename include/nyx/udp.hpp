#pragma once

/** @file udp.hpp
 *  Blocking UDP socket (Windows Winsock / BSD sockets).
 *  Several Connections may share one socket (shared_ptr).
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nyx {

class UdpSocket {
public:
  UdpSocket();
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = default;
  UdpSocket& operator=(const UdpSocket&) = default;
  UdpSocket(UdpSocket&& other) noexcept = default;
  UdpSocket& operator=(UdpSocket&& other) noexcept = default;

  /** Binds to host:port. Port 0 lets the OS choose. @return false on bind error. */
  bool bind(const std::string& host, uint16_t port, std::string* err = nullptr);

  bool send_to(const ByteBuffer& data, const std::string& host, uint16_t port);

  /** Receives a datagram. timeout_ms < 0 = no timeout; 0 = poll. */
  std::optional<ByteBuffer> recv_from(std::string& host, uint16_t& port, int timeout_ms = -1);

  /** Local port after bind. */
  uint16_t local_port() const;

  /** Bind + join multicast group; optional iface IPv4 (empty = auto). */
  bool bind_multicast_listener(const std::string& group,
                               uint16_t port,
                               std::string* err = nullptr,
                               const std::string& iface_ipv4 = {});

  bool enable_broadcast(std::string* err = nullptr);

  /** Set multicast TX iface; re-joins membership when group was joined. */
  bool set_multicast_interface(const std::string& ipv4, std::string* err = nullptr);

private:
  struct State {
    uintptr_t sock = static_cast<uintptr_t>(-1);
    uint16_t local_port = 0;
    std::string mcast_group;
    uint32_t mcast_iface_addr = 0;
  };

  std::shared_ptr<State> state_;
  static bool platform_init();
};

} // namespace nyx
