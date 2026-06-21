#pragma once

/** @file mdns.hpp
 *  LAN discovery через multicast beacon Nyx, фаза 6.
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

/** Узел, найденный в локальной сети. */
struct LanPeer {
  std::string instance;
  std::string host;
  uint16_t port = 0;
  std::string user_id_short;
};

/** Периодическая публикация и опрос mDNS. */
class MdnsLan {
 public:
  MdnsLan() = default;
  ~MdnsLan();

  MdnsLan(const MdnsLan&) = delete;
  MdnsLan& operator=(const MdnsLan&) = delete;

  /** Настраивает сокет для приёма discovery-beacon (bind + multicast join). */
  static bool setup_socket(UdpSocket& socket, std::string* err = nullptr);

  /** Фоновые announce каждые ~3 с. */
  void start_advertising(UdpSocket socket, Profile profile, uint16_t port,
                         std::string host_ip);
  void stop_advertising();

  /** Опрос LAN, сбор ответов beacon Nyx. */
  static std::vector<LanPeer> browse(UdpSocket& socket, int timeout_ms = 3000);

  /** Одно announce (для тестов). */
  static bool send_announcement(UdpSocket& socket, const Profile& profile,
                                uint16_t port, const std::string& host_ip);

  /** Разбор beacon-пакета (тесты). */
  static std::optional<LanPeer> parse_beacon(const ByteBuffer& data,
                                             const std::string& from_host);

 private:
  std::atomic<bool> running_{false};
  std::thread thread_;
  UdpSocket advert_socket_;
};

}  // namespace nyx
