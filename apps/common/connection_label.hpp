#pragma once

#include <string>

namespace nyx_app {

enum class ConnectionVia {
  None,
  LanDirect,
  Rendezvous,
  Incoming,
  Group,
};

std::string connection_label(ConnectionVia via, const std::string& peer_host);

}
