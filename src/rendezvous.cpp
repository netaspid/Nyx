#include "nyx/connection.hpp"
#include "nyx/nat.hpp"

#include "nyx/util.hpp"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <chrono>

namespace nyx {

EndpointHint make_hint(const std::string& host, uint16_t port) {
  EndpointHint h{};
  in_addr addr{};
  inet_pton(AF_INET, host.c_str(), &addr);
  h.ip[10] = 0xff;
  h.ip[11] = 0xff;
  std::memcpy(h.ip.data() + 12, &addr, 4);
  h.port = port;
  random_bytes(h.nonce.data(), h.nonce.size());
  return h;
}

RendezvousClient::RendezvousClient(UdpSocket socket, std::string server_host,
                                   uint16_t server_port)
    : socket_(std::move(socket)),
      server_host_(std::move(server_host)),
      server_port_(server_port) {}

bool RendezvousClient::send_msg(PacketType type, const ByteBuffer& payload) {
  auto wire = Frame::make(type, 0, 0, payload).encode();
  return socket_.send_to(wire, server_host_, server_port_);
}

bool RendezvousClient::register_token(const InviteToken& token) {
  RendezvousMessage msg;
  msg.kind = RendezvousKind::Register;
  msg.token = token;
  msg.hint = make_public_hint(socket_, guess_lan_ipv4(), socket_.local_port());
  return send_msg(PacketType::RendezvousRegister, msg.encode());
}

std::optional<EndpointHint> RendezvousClient::lookup(const InviteToken& token) {
  RendezvousMessage msg;
  msg.kind = RendezvousKind::Lookup;
  msg.token = token;
  if (!send_msg(PacketType::RendezvousLookup, msg.encode())) return std::nullopt;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string host;
    uint16_t port = 0;
    auto data = socket_.recv_from(host, port, 200);
    if (!data) continue;
    if (host != server_host_ || port != server_port_) continue;

    auto frame = Frame::decode(data->data(), data->size());
    if (!frame || frame->header.packet_type != PacketType::RendezvousResponse) {
      continue;
    }
    auto resp =
        RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
    if (!resp || resp->kind != RendezvousKind::Response) continue;
    if (resp->hint.port == 0) continue;
    return resp->hint;
  }
  return std::nullopt;
}

}  // namespace nyx
