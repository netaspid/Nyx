#pragma once

#include "nyx/network_config.hpp"
#include "nyx/proto.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <optional>
#include <vector>

namespace nyx {

class RendezvousPool {
public:
  explicit RendezvousPool(UdpSocket socket);

  void set_servers(const std::vector<RendezvousServer>& servers);


  bool register_token(const InviteToken& token);


  bool unregister_token(const InviteToken& token);


  std::optional<EndpointHint> lookup(const InviteToken& token);


  bool probe_server(const RendezvousServer& server, int timeout_ms = 2000);

  UdpSocket& socket() { return socket_; }

private:
  bool send_to_server(const RendezvousServer& server, PacketType type, const ByteBuffer& payload);
  std::optional<EndpointHint>
  lookup_on(const RendezvousServer& server, const InviteToken& token, int timeout_ms);

  UdpSocket socket_;
  std::vector<RendezvousServer> servers_;
};

bool register_token_on(UdpSocket& socket,
                       const std::vector<RendezvousServer>& servers,
                       const InviteToken& token);

bool unregister_token_on(UdpSocket& socket,
                         const std::vector<RendezvousServer>& servers,
                         const InviteToken& token);

}
