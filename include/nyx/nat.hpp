#pragma once

#include "nyx/proto.hpp"
#include "nyx/udp.hpp"

#include <optional>
#include <string>

namespace nyx {

std::string guess_lan_ipv4();

void set_lan_ipv4_override(const std::string& ipv4);
std::string lan_ipv4_override();

void hole_punch(UdpSocket& sock, const EndpointHint& hint);
void hole_punch_burst(UdpSocket& sock, const EndpointHint& hint, int packets = 4);

std::optional<EndpointHint>
stun_external_endpoint(UdpSocket& sock,
                       const std::string& stun_host = "stun.l.google.com",
                       uint16_t stun_port = 19302,
                       int timeout_ms = 800);

EndpointHint make_public_hint(UdpSocket& sock, const std::string& fallback_host, uint16_t port);

bool is_lan_ipv4(const std::string& host);

} // namespace nyx
