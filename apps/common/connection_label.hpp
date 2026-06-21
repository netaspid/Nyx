#pragma once

/** Метка типа соединения для UI и логов (GUI + CLI). */

#include <string>

namespace nyx_app {

enum class ConnectionVia {
  None,
  LanDirect,
  Rendezvous,
  Incoming,
  Group,
};

/** Человекочитаемая метка: LAN, Интернет, Поле, … */
std::string connection_label(ConnectionVia via, const std::string& peer_host);

}  // namespace nyx_app
