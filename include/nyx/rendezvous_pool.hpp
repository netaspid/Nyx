#pragma once

/** @file rendezvous_pool.hpp
 *  Несколько bootstrap-серверов: register на все, lookup с failover.
 */

#include "nyx/network_config.hpp"
#include "nyx/proto.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <optional>
#include <vector>

namespace nyx {

/** Клиент с поддержкой списка rendezvous. */
class RendezvousPool {
 public:
  explicit RendezvousPool(UdpSocket socket);

  void set_servers(const std::vector<RendezvousServer>& servers);

  /** Register token на каждом сервере из списка. */
  bool register_token(const InviteToken& token);

  /** Lookup: опрашивает серверы по порядку, первый успешный hint. */
  std::optional<EndpointHint> lookup(const InviteToken& token);

  /** Проверка UDP-доступности (Register с пустым token не шлём — только lookup ping). */
  bool probe_server(const RendezvousServer& server, int timeout_ms = 2000);

  UdpSocket& socket() { return socket_; }

 private:
  bool send_to_server(const RendezvousServer& server, PacketType type,
                      const ByteBuffer& payload);
  std::optional<EndpointHint> lookup_on(const RendezvousServer& server,
                                         const InviteToken& token, int timeout_ms);

  UdpSocket socket_;
  std::vector<RendezvousServer> servers_;
};

}  // namespace nyx
