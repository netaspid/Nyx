#include "nyx/nat.hpp"

#include "nyx/connection.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace nyx {

namespace {

uint32_t read_u32_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t read_u16_be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

void write_u32_be_to(ByteBuffer& buf, uint32_t v, std::size_t off) {
  buf[off] = static_cast<uint8_t>((v >> 24) & 0xff);
  buf[off + 1] = static_cast<uint8_t>((v >> 16) & 0xff);
  buf[off + 2] = static_cast<uint8_t>((v >> 8) & 0xff);
  buf[off + 3] = static_cast<uint8_t>(v & 0xff);
}

}  // namespace

namespace {
std::string g_lan_ipv4_override;
std::mutex g_lan_ipv4_mutex;
}  // namespace

void set_lan_ipv4_override(const std::string& ipv4) {
  std::lock_guard lock(g_lan_ipv4_mutex);
  g_lan_ipv4_override = ipv4;
}

std::string lan_ipv4_override() {
  std::lock_guard lock(g_lan_ipv4_mutex);
  return g_lan_ipv4_override;
}

std::string guess_lan_ipv4() {
  {
    const std::string over = lan_ipv4_override();
    if (!over.empty() && over != "0.0.0.0" && over != "127.0.0.1") return over;
  }
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return "127.0.0.1";
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
    closesocket(s);
    return "127.0.0.1";
  }
  sockaddr_in local{};
  int len = sizeof(local);
  getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
  closesocket(s);
  char buf[64] = {};
  inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
  return buf;
#else
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return "127.0.0.1";
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
  if (connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
    close(s);
    return "127.0.0.1";
  }
  sockaddr_in local{};
  socklen_t len = sizeof(local);
  getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
  close(s);
  char buf[64] = {};
  inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
  return buf;
#endif
}

void hole_punch_burst(UdpSocket& sock, const EndpointHint& hint, int packets) {
  const ByteBuffer probe = {'D', 'N', 'E', 'T', '-', 'P', 'U', 'N', 'C', 'H'};
  char host[64] = {};
  std::snprintf(host, sizeof(host), "%u.%u.%u.%u", hint.ip[12], hint.ip[13],
                hint.ip[14], hint.ip[15]);
  if (packets < 1) packets = 1;
  for (int i = 0; i < packets; ++i) {
    sock.send_to(probe, host, hint.port);
  }
}

void hole_punch(UdpSocket& sock, const EndpointHint& hint) {
  for (int round = 0; round < 5; ++round) {
    hole_punch_burst(sock, hint, 8);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

std::optional<EndpointHint> stun_external_endpoint(UdpSocket& sock,
                                                   const std::string& stun_host,
                                                   uint16_t stun_port,
                                                   int timeout_ms) {
  ByteBuffer req(20, 0);
  req[0] = 0x00;
  req[1] = 0x01;
  write_u32_be_to(req, 0x2112A442, 4);
  random_bytes(req.data() + 8, 12);

  if (!sock.send_to(req, stun_host, stun_port)) return std::nullopt;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string from;
    uint16_t from_port = 0;
    auto resp = sock.recv_from(from, from_port, 200);
    if (!resp || resp->size() < 20) continue;
    if (from != stun_host || from_port != stun_port) continue;
    if ((*resp)[0] != 0x01 || (*resp)[1] != 0x01) continue;

    std::size_t off = 20;
    while (off + 4 <= resp->size()) {
      const uint16_t type = read_u16_be(resp->data() + off);
      const uint16_t len = read_u16_be(resp->data() + off + 2);
      off += 4;
      if (off + len > resp->size()) break;

      if (type == 0x0020 && len >= 8) {
        const uint8_t family = (*resp)[off + 1];
        if (family == 0x01) {
          const uint16_t xport = read_u16_be(resp->data() + off + 2);
          const uint32_t xaddr = read_u32_be(resp->data() + off + 4);
          const uint32_t cookie = read_u32_be(req.data() + 4);
          const uint16_t port = static_cast<uint16_t>(xport ^ (cookie >> 16));
          const uint32_t addr = xaddr ^ cookie;

          EndpointHint hint{};
          hint.ip[10] = 0xff;
          hint.ip[11] = 0xff;
          std::memcpy(hint.ip.data() + 12, &addr, 4);
          hint.port = port;
          random_bytes(hint.nonce.data(), hint.nonce.size());
          return hint;
        }
      }
      off += len;
      if (len % 4 != 0) off += 4 - (len % 4);
    }
  }
  return std::nullopt;
}

EndpointHint make_public_hint(UdpSocket& sock, const std::string& fallback_host,
                              uint16_t port) {
  if (auto stun = stun_external_endpoint(sock)) {
    stun->port = port;
    return *stun;
  }
  return make_hint(fallback_host.empty() ? guess_lan_ipv4() : fallback_host, port);
}

bool is_lan_ipv4(const std::string& host) {
  if (host == "127.0.0.1" || host == "localhost") return true;
  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return false;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr.s_addr);
  if (b[0] == 10) return true;
  if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
  if (b[0] == 192 && b[1] == 168) return true;
  return false;
}

}  // namespace nyx
