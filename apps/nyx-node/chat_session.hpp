#pragma once

/** @file chat_session.hpp
 *  Интерактивный чат: ChatMessage, история, события connect/disconnect.
 */

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

namespace nyx_node {

/** Запуск чата после установленного P2P-соединения. */
void run_chat_session(nyx::Connection& connection, const nyx::Profile& profile,
                      bool incoming_connection);

}  // namespace nyx_node
