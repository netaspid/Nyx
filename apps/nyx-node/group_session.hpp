#pragma once

/** @file group_session.hpp
 *  CLI field sessions: hub (creator) and join (member).
 */

#include "cli.hpp"

#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

/** Creates a field and prints the group_id + invite token. */
int run_group_create(const std::string& name, const NodeConfig& config);

/** Field hub: rendezvous + GroupHub + interactive chat. */
int run_group_hub(const std::string& group_id_hex, const NodeConfig& config);

/** Joins a field by invite token. */
int run_group_join(const std::string& token_hex, const NodeConfig& config);

}  // namespace nyx_node
