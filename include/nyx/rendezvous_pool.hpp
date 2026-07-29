#pragma once

/** @file rendezvous_pool.hpp
 *  Multiple bootstrap servers: register on all, lookup with failover.
 */

#include "nyx/network_config.hpp"
#include "nyx/proto.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <optional>
#include <vector>

namespace nyx {

/** Client supporting a rendezvous server list. */
class RendezvousPool {
public:
  explicit RendezvousPool(UdpSocket socket);

  void set_servers(const std::vector<RendezvousServer>& servers);

  /** Registers the token on every server in the list. */
  bool register_token(const InviteToken& token);

  /** Unregisters the token from bootstrap (when listen/hub stops). */
  bool unregister_token(const InviteToken& token);

  /** Lookup: queries servers in order, first successful hint wins. */
  std::optional<EndpointHint> lookup(const InviteToken& token);

  /** UDP reachability check (no empty-token Register; lookup ping only). */
  bool probe_server(const RendezvousServer& server, int timeout_ms = 2000);

  UdpSocket& socket() { return socket_; }

private:
  bool send_to_server(const RendezvousServer& server, PacketType type, const ByteBuffer& payload);
  std::optional<EndpointHint>
  lookup_on(const RendezvousServer& server, const InviteToken& token, int timeout_ms);

  UdpSocket socket_;
  std::vector<RendezvousServer> servers_;
};

/** Register/unregister via an already open UDP socket (hub/listen refresh). */
bool register_token_on(UdpSocket& socket,
                       const std::vector<RendezvousServer>& servers,
                       const InviteToken& token);
/** Removes the invite from bootstrap when hub/listen stops. */
bool unregister_token_on(UdpSocket& socket,
                         const std::vector<RendezvousServer>& servers,
                         const InviteToken& token);

} // namespace nyx
