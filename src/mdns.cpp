#include "nyx/mdns.hpp"

#include "nyx/identity.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace nyx {

namespace {

constexpr char kMagic[] = "NYX1";
constexpr std::size_t kMagicLen = 4;
/** Собственный multicast Nyx (не 5353 — там системный mDNS/Bonjour). */
constexpr char kDiscoveryGroup[] = "239.255.77.77";
constexpr uint16_t kDiscoveryPort = 34779;

std::string sanitize_instance(const std::string& nickname) {
  std::string out;
  for (char c : nickname) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-') {
      out.push_back(c);
    } else if (c == ' ' || c == '_') {
      out.push_back('-');
    }
  }
  if (out.empty()) out = "nyx-peer";
  return out;
}

uint32_t ipv4_to_u32(const std::string& host) {
  in_addr addr{};
  if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return 0;
  uint32_t v = 0;
  std::memcpy(&v, &addr, 4);
  return v;
}

std::string u32_to_ipv4(uint32_t v) {
  in_addr addr{};
  std::memcpy(&addr, &v, 4);
  char buf[64] = {};
  inet_ntop(AF_INET, &addr, buf, sizeof(buf));
  return buf;
}

ByteBuffer encode_beacon(const Profile& profile, uint16_t port, const std::string& host_ip) {
  const std::string instance = sanitize_instance(profile.nickname);
  const std::string id_short = short_user_id(profile.user_id());
  const uint32_t ip = ipv4_to_u32(host_ip);

  ByteBuffer out;
  out.insert(out.end(), kMagic, kMagic + kMagicLen);
  write_u16_le(out, port);
  write_u16_le(out, static_cast<uint16_t>(instance.size()));
  write_u16_le(out, static_cast<uint16_t>(id_short.size()));
  write_u32_le(out, ip);
  out.insert(out.end(), instance.begin(), instance.end());
  out.insert(out.end(), id_short.begin(), id_short.end());
  return out;
}

std::optional<LanPeer> decode_beacon(const ByteBuffer& data, const std::string& from_host) {
  if (data.size() < kMagicLen + 2 + 2 + 2 + 4) return std::nullopt;
  if (std::memcmp(data.data(), kMagic, kMagicLen) != 0) return std::nullopt;

  std::size_t off = kMagicLen;
  const uint16_t port = read_u16_le(data.data() + off);
  off += 2;
  const uint16_t inst_len = read_u16_le(data.data() + off);
  off += 2;
  const uint16_t id_len = read_u16_le(data.data() + off);
  off += 2;
  const uint32_t ip = read_u32_le(data.data() + off);
  off += 4;
  if (off + inst_len + id_len > data.size()) return std::nullopt;

  LanPeer peer;
  peer.instance.assign(reinterpret_cast<const char*>(data.data() + off), inst_len);
  off += inst_len;
  peer.user_id_short.assign(reinterpret_cast<const char*>(data.data() + off), id_len);
  peer.port = port;
  peer.host = ip != 0 ? u32_to_ipv4(ip) : from_host;
  return peer;
}

}  // namespace

bool MdnsLan::setup_socket(UdpSocket& socket, std::string* err) {
  return socket.bind_multicast_listener(kDiscoveryGroup, kDiscoveryPort, err);
}

bool MdnsLan::send_announcement(UdpSocket& socket, const Profile& profile, uint16_t port,
                                const std::string& host_ip) {
  const auto wire = encode_beacon(profile, port, host_ip);
  return socket.send_to(wire, kDiscoveryGroup, kDiscoveryPort);
}

std::optional<LanPeer> MdnsLan::parse_beacon(const ByteBuffer& data,
                                             const std::string& from_host) {
  return decode_beacon(data, from_host);
}

MdnsLan::~MdnsLan() { stop_advertising(); }

void MdnsLan::start_advertising(UdpSocket socket, Profile profile, uint16_t port,
                                std::string host_ip) {
  stop_advertising();
  advert_socket_ = std::move(socket);
  running_.store(true);
  thread_ = std::thread([this, profile = std::move(profile), port,
                         host_ip = std::move(host_ip)]() mutable {
    while (running_.load()) {
      send_announcement(advert_socket_, profile, port, host_ip);
      for (int i = 0; i < 10 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void MdnsLan::stop_advertising() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

std::vector<LanPeer> MdnsLan::browse(UdpSocket& socket, int timeout_ms) {
  std::map<std::string, LanPeer> seen;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string host;
    uint16_t port = 0;
    auto pkt = socket.recv_from(host, port, 200);
    if (!pkt) continue;
    if (auto peer = decode_beacon(*pkt, host)) {
      const std::string key = peer->host + ':' + std::to_string(peer->port);
      seen[key] = *peer;
    }
  }

  std::vector<LanPeer> out;
  out.reserve(seen.size());
  for (auto& [_, p] : seen) out.push_back(std::move(p));
  std::sort(out.begin(), out.end(),
            [](const LanPeer& a, const LanPeer& b) { return a.instance < b.instance; });
  return out;
}

}  // namespace nyx
