#pragma once

#include "nyx/proto.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>

namespace nyx {

struct RendezvousServerConfig {
  std::string bind_host = "0.0.0.0";
  uint16_t bind_port = 3478;
  std::chrono::minutes entry_ttl {5};

  std::uint32_t rate_limit_per_minute = 120;
};

class RendezvousRegistry {
public:
  explicit RendezvousRegistry(RendezvousServerConfig config);


  std::optional<ByteBuffer> handle_datagram(const std::string& client_ip,
                                            const ByteBuffer& datagram);

  std::size_t entry_count() const { return registry_.size(); }

private:
  struct Entry {
    EndpointHint hint;
    std::chrono::steady_clock::time_point expires;
  };

  bool allow_request(const std::string& client_ip);

  RendezvousServerConfig config_;
  std::map<std::string, Entry> registry_;
  std::map<std::string, std::pair<std::uint32_t, std::chrono::steady_clock::time_point>>
      rate_buckets_;
};

}
