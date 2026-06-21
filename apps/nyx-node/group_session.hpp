#pragma once

/** @file group_session.hpp
 *  CLI-сессии поля: hub (создатель) и join (участник).
 */

#include "cli.hpp"

#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

/** Создаёт поле и выводит group_id + invite token. */
int run_group_create(const std::string& name, const NodeConfig& config);

/** Hub поля: rendezvous + GroupHub + интерактивный чат. */
int run_group_hub(const std::string& group_id_hex, const NodeConfig& config);

/** Вход в поле по invite token. */
int run_group_join(const std::string& token_hex, const NodeConfig& config);

}  // namespace nyx_node
