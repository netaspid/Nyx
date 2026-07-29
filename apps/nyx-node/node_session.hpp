#pragma once

/** @file node_session.hpp
 *  listen/connect scenarios and the interactive chat.
 */

#include "cli.hpp"

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

/** Listener mode: rendezvous register, accept, chat. */
int run_listen(const NodeConfig& config);

/** Connect by invite token, then chat. */
int run_connect(const std::string& token_hex, const NodeConfig& config);

/** Direct connect by host:port (LAN). */
int run_connect_peer(const NodeConfig& config);

/** LAN node discovery. */
int run_browse(int timeout_ms);

} // namespace nyx_node
