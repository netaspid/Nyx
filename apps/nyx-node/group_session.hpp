#pragma once

#include "cli.hpp"

#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

int run_group_create(const std::string& name, const NodeConfig& config);

int run_group_hub(const std::string& group_id_hex, const NodeConfig& config);

int run_group_join(const std::string& token_hex, const NodeConfig& config);

}
