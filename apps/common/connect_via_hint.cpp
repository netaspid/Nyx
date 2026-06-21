#include "connect_via_hint.hpp"

#include "nyx/nat.hpp"

namespace nyx_app {

HintConnectResult connect_via_rendezvous_hint(nyx::UdpSocket socket,
                                              const nyx::EndpointHint& hint) {
  HintConnectResult out;
  out.host = hint.host_string();
  out.port = hint.port;

  if (!nyx::is_lan_ipv4(out.host)) {
    nyx::hole_punch(socket, hint);
  }

  auto conn = nyx::Connection::connect_initiator(std::move(socket), out.host, out.port);
  if (conn) out.connection = std::move(*conn);
  return out;
}

}  // namespace nyx_app
