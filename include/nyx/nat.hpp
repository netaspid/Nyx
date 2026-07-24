#pragma once

/** @file nat.hpp
 *  NAT traversal: hole punch, STUN (фаза 6).
 */

#include "nyx/proto.hpp"
#include "nyx/udp.hpp"

#include <optional>
#include <string>

namespace nyx {

/** Лучший локальный IPv4 для LAN (не 127.0.0.1). */
std::string guess_lan_ipv4();

/** Prefer this IPv4 for LAN advertise / multicast IF (e.g. Android Wi‑Fi). Empty = auto. */
void set_lan_ipv4_override(const std::string& ipv4);
std::string lan_ipv4_override();

void hole_punch(UdpSocket& sock, const EndpointHint& hint);
void hole_punch_burst(UdpSocket& sock, const EndpointHint& hint, int packets = 4);

/** STUN Binding Request → внешний endpoint (best effort). */
std::optional<EndpointHint> stun_external_endpoint(UdpSocket& sock,
                                                   const std::string& stun_host = "stun.l.google.com",
                                                   uint16_t stun_port = 19302,
                                                   int timeout_ms = 800);

/** Hint для rendezvous: LAN IP или STUN, если доступен. */
EndpointHint make_public_hint(UdpSocket& sock, const std::string& fallback_host,
                              uint16_t port);

/** Частный или loopback IPv4 (для индикатора LAN в UI). */
bool is_lan_ipv4(const std::string& host);

}  // namespace nyx
