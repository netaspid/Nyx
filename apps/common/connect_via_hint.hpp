#pragma once

/** Initiator connect via a rendezvous EndpointHint (LAN punch + handshake). */

#include "nyx/connection.hpp"
#include "nyx/proto.hpp"
#include "nyx/transport.hpp"

#include <optional>
#include <string>

namespace nyx_app {

struct HintConnectResult {
  std::optional<nyx::Connection> connection;
  std::string host;
  uint16_t port = 0;
};

/** Hole punch outside the LAN, then Noise handshake. The socket is moved in. */
HintConnectResult connect_via_rendezvous_hint(nyx::UdpSocket socket,
                                              const nyx::EndpointHint& hint);

}  // namespace nyx_app
