#include "nyx/udp.hpp"

#include "nyx/nat.hpp"

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace nyx {

namespace {

bool g_net_init = false;

bool resolve_host(const std::string& host, uint16_t port, sockaddr_storage& out,
                  socklen_t& out_len) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
    return false;
  }
  std::memcpy(&out, res->ai_addr, res->ai_addrlen);
  out_len = static_cast<socklen_t>(res->ai_addrlen);
  freeaddrinfo(res);
  return true;
}

void close_socket(uintptr_t sock) {
  if (sock == static_cast<uintptr_t>(kInvalidSocket)) return;
#ifdef _WIN32
  closesocket(static_cast<socket_t>(sock));
#else
  close(static_cast<socket_t>(sock));
#endif
}

in_addr resolve_mcast_iface(const std::string& iface_ipv4) {
  in_addr iface{};
  iface.s_addr = INADDR_ANY;
  std::string ip = iface_ipv4;
  if (ip.empty()) ip = lan_ipv4_override();
  if (ip.empty()) {
    const std::string guessed = guess_lan_ipv4();
    if (guessed != "127.0.0.1" && guessed != "0.0.0.0") ip = guessed;
  }
  if (!ip.empty() && ip != "127.0.0.1" && ip != "0.0.0.0") {
    inet_pton(AF_INET, ip.c_str(), &iface);
  }
  return iface;
}

bool join_mcast_group(socket_t s, const std::string& group, in_addr iface, std::string* err) {
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
  mreq.imr_interface = iface;
  if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                 sizeof(mreq)) != 0) {
    if (err) *err = "IP_ADD_MEMBERSHIP failed";
    return false;
  }
  return true;
}

bool leave_mcast_group(socket_t s, const std::string& group, uint32_t iface_nbo) {
  if (group.empty()) return true;
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
  mreq.imr_interface.s_addr = iface_nbo;
  setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
             sizeof(mreq));
  return true;
}

}  // namespace

bool UdpSocket::platform_init() {
  if (g_net_init) return true;
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
  g_net_init = true;
  return true;
}

void UdpSocket::platform_shutdown() {
#ifdef _WIN32
  if (g_net_init) WSACleanup();
#endif
  g_net_init = false;
}

UdpSocket::UdpSocket() : state_(std::make_shared<State>()) {
  platform_init();
  state_->sock = static_cast<uintptr_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
}

UdpSocket::~UdpSocket() {
  if (state_ && state_.use_count() == 1) {
    close_socket(state_->sock);
    state_->sock = static_cast<uintptr_t>(kInvalidSocket);
  }
}

uint16_t UdpSocket::local_port() const {
  return state_ ? state_->local_port : 0;
}

bool UdpSocket::bind(const std::string& host, uint16_t port, std::string* err) {
  if (!state_) {
    if (err) *err = "invalid socket";
    return false;
  }
  auto s = static_cast<socket_t>(state_->sock);
  if (s == kInvalidSocket) {
    if (err) *err = "invalid socket";
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (host.empty() || host == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind failed";
    return false;
  }
  {
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf),
               sizeof(sndbuf));
  }
  socklen_t len = sizeof(addr);
  getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  state_->local_port = ntohs(addr.sin_port);
  return true;
}

bool UdpSocket::send_to(const ByteBuffer& data, const std::string& host, uint16_t port) {
  if (!state_) return false;
  sockaddr_storage addr{};
  socklen_t len = 0;
  if (!resolve_host(host, port, addr, len)) return false;
  const int sent = sendto(static_cast<socket_t>(state_->sock),
                          reinterpret_cast<const char*>(data.data()),
                          static_cast<int>(data.size()), 0,
                          reinterpret_cast<sockaddr*>(&addr), len);
  return sent == static_cast<int>(data.size());
}

std::optional<ByteBuffer> UdpSocket::recv_from(std::string& host, uint16_t& port,
                                                int timeout_ms) {
  if (!state_) return std::nullopt;
  auto s = static_cast<socket_t>(state_->sock);
  if (timeout_ms >= 0) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(static_cast<int>(s) + 1, &fds, nullptr, nullptr, &tv) <= 0) {
      return std::nullopt;
    }
  }

  ByteBuffer buf(65535);
  sockaddr_storage from{};
  socklen_t from_len = sizeof(from);
  const int n = recvfrom(s, reinterpret_cast<char*>(buf.data()),
                         static_cast<int>(buf.size()), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
  if (n <= 0) return std::nullopt;
  buf.resize(static_cast<std::size_t>(n));

  char host_buf[64] = {};
  const auto* sin = reinterpret_cast<const sockaddr_in*>(&from);
  inet_ntop(AF_INET, &sin->sin_addr, host_buf, sizeof(host_buf));
  host = host_buf;
  port = ntohs(sin->sin_port);
  return buf;
}

bool UdpSocket::bind_multicast_listener(const std::string& group, uint16_t port,
                                        std::string* err, const std::string& iface_ipv4) {
  if (!state_ || state_->sock == static_cast<uintptr_t>(kInvalidSocket)) {
    if (err) *err = "invalid socket";
    return false;
  }
  auto s = static_cast<socket_t>(state_->sock);

  int reuse = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse)) != 0) {
    if (err) *err = "SO_REUSEADDR failed";
    return false;
  }
  enable_broadcast(nullptr);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (err) *err = "bind failed";
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  state_->local_port = ntohs(addr.sin_port);

  const in_addr iface = resolve_mcast_iface(iface_ipv4);
  if (!join_mcast_group(s, group, iface, err)) {
    in_addr any{};
    any.s_addr = INADDR_ANY;
    if (!join_mcast_group(s, group, any, err)) return false;
    state_->mcast_iface_addr = any.s_addr;
  } else {
    state_->mcast_iface_addr = iface.s_addr;
  }
  state_->mcast_group = group;

  setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface),
             sizeof(iface));

  int loop = 1;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
             sizeof(loop));
  unsigned char ttl = 1;
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));

  return true;
}

bool UdpSocket::enable_broadcast(std::string* err) {
  if (!state_ || state_->sock == static_cast<uintptr_t>(kInvalidSocket)) {
    if (err) *err = "invalid socket";
    return false;
  }
  auto s = static_cast<socket_t>(state_->sock);
  int on = 1;
  if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on)) !=
      0) {
    if (err) *err = "SO_BROADCAST failed";
    return false;
  }
  return true;
}

bool UdpSocket::set_multicast_interface(const std::string& ipv4, std::string* err) {
  if (!state_ || state_->sock == static_cast<uintptr_t>(kInvalidSocket)) {
    if (err) *err = "invalid socket";
    return false;
  }
  if (ipv4.empty()) return true;
  auto s = static_cast<socket_t>(state_->sock);
  in_addr iface{};
  if (inet_pton(AF_INET, ipv4.c_str(), &iface) != 1) {
    if (err) *err = "bad multicast interface IP";
    return false;
  }

  if (!state_->mcast_group.empty() && state_->mcast_iface_addr != iface.s_addr) {
    leave_mcast_group(s, state_->mcast_group, state_->mcast_iface_addr);
    std::string join_err;
    if (!join_mcast_group(s, state_->mcast_group, iface, &join_err)) {
      in_addr any{};
      any.s_addr = INADDR_ANY;
      if (!join_mcast_group(s, state_->mcast_group, any, err)) return false;
      state_->mcast_iface_addr = any.s_addr;
    } else {
      state_->mcast_iface_addr = iface.s_addr;
    }
  }

  if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface),
                 sizeof(iface)) != 0) {
    if (err) *err = "IP_MULTICAST_IF failed";
    return false;
  }
  return true;
}

bool UdpSocket::enable_lan_multicast(std::string* err) {
  return bind_multicast_listener("224.0.0.251", 5353, err);
}

}  // namespace nyx
