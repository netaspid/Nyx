#pragma once

/** @file node_session.hpp
 *  Сценарии listen/connect и интерактивный чат (фаза 1 roadmap).
 */

#include "cli.hpp"

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"

#include <string>

namespace nyx_node {

/** Режим слушателя: register на rendezvous, accept, чат. */
int run_listen(const NodeConfig& config);

/** Подключение по invite token, чат. */
int run_connect(const std::string& token_hex, const NodeConfig& config);

/** Прямое подключение по host:port (LAN). */
int run_connect_peer(const NodeConfig& config);

/** Поиск узлов в локальной сети. */
int run_browse(int timeout_ms);

}  // namespace nyx_node
