#pragma once

/** @file chat_session.hpp
 *  Interactive chat: ChatMessage, history, connect/disconnect events.
 */

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

namespace nyx_node {

/** Starts the chat over an established P2P connection. */
void run_chat_session(nyx::Connection& connection,
                      const nyx::Profile& profile,
                      bool incoming_connection);

} // namespace nyx_node
