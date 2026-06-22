#include "nyx/rendezvous_server.hpp"

#include "nyx/log.hpp"
#include "nyx/util.hpp"

namespace nyx {

RendezvousRegistry::RendezvousRegistry(RendezvousServerConfig config)
    : config_(std::move(config)) {}

bool RendezvousRegistry::allow_request(const std::string& client_ip) {
  if (config_.rate_limit_per_minute == 0) return true;
  const auto now = std::chrono::steady_clock::now();
  auto& bucket = rate_buckets_[client_ip];
  if (bucket.second + std::chrono::minutes(1) < now) {
    bucket = {1, now};
    return true;
  }
  if (bucket.first >= config_.rate_limit_per_minute) return false;
  ++bucket.first;
  return true;
}

std::optional<ByteBuffer> RendezvousRegistry::handle_datagram(
    const std::string& client_ip, const ByteBuffer& datagram) {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = registry_.begin(); it != registry_.end();) {
    if (it->second.expires < now)
      it = registry_.erase(it);
    else
      ++it;
  }

  if (!allow_request(client_ip)) {
    log_info("rendezvous rate limit: " + client_ip);
    return std::nullopt;
  }

  auto frame = Frame::decode(datagram.data(), datagram.size());
  if (!frame) return std::nullopt;

  if (frame->header.packet_type == PacketType::RendezvousRegister) {
    auto msg = RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
    if (!msg) return std::nullopt;
    const std::string key = to_hex(msg->token.data(), msg->token.size());
    if (msg->kind == RendezvousKind::Unregister) {
      registry_.erase(key);
      log_info("rendezvous unregister " + key.substr(0, 8) + "… from " + client_ip);
      return std::nullopt;
    }
    if (msg->kind != RendezvousKind::Register) return std::nullopt;
    Entry e{msg->hint, now + config_.entry_ttl};
    registry_[key] = e;
    log_info("rendezvous register " + key.substr(0, 8) + "… from " + client_ip);
    return std::nullopt;
  }

  if (frame->header.packet_type == PacketType::RendezvousLookup) {
    auto msg = RendezvousMessage::decode(frame->payload.data(), frame->payload.size());
    if (!msg || msg->kind != RendezvousKind::Lookup) return std::nullopt;
    RendezvousMessage resp;
    const std::string key = to_hex(msg->token.data(), msg->token.size());
    if (registry_.count(key)) {
      resp.kind = RendezvousKind::Response;
      resp.hint = registry_[key].hint;
      log_info("rendezvous lookup hit " + key.substr(0, 8) + "…");
    } else {
      resp.kind = RendezvousKind::NotFound;
      log_info("rendezvous lookup miss " + key.substr(0, 8) + "…");
    }
    const auto payload = resp.encode();
    return Frame::make(PacketType::RendezvousResponse, 0, 0, payload).encode();
  }

  return std::nullopt;
}

}  // namespace nyx
