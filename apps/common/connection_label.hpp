#pragma once

/** Connection type label for UI and logs (GUI + CLI). */

#include <string>

namespace nyx_app {

enum class ConnectionVia {
  None,
  LanDirect,
  Rendezvous,
  Incoming,
  Group,
};

/** Human-readable label: LAN, Internet, Field, ... */
std::string connection_label(ConnectionVia via, const std::string& peer_host);

}  // namespace nyx_app
