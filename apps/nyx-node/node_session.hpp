#pragma once

#include "cli.hpp"

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

int run_listen(const NodeConfig& config);

int run_connect(const std::string& token_hex, const NodeConfig& config);

int run_connect_peer(const NodeConfig& config);

int run_browse(int timeout_ms);

} // namespace nyx_node
