#include "connection_label.hpp"

#include "nyx/nat.hpp"

namespace nyx_app {

std::string connection_label(ConnectionVia via, const std::string& peer_host) {
  switch (via) {
    case ConnectionVia::LanDirect:
      return "LAN";
    case ConnectionVia::Rendezvous:
      return nyx::is_lan_ipv4(peer_host) ? "LAN (token)" : "Интернет";
    case ConnectionVia::Incoming:
      return nyx::is_lan_ipv4(peer_host) ? "LAN (входящее)" : "Интернет (входящее)";
    case ConnectionVia::Group:
      return "Поле";
    default:
      return {};
  }
}

}  // namespace nyx_app
