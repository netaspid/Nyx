#pragma once

/** @file cli.hpp
 *  Разбор аргументов командной строки nyx-node.
 */

#include <cstdint>
#include <string>

namespace nyx_node {

/** Параметры запуска узла. */
struct NodeConfig {
  std::string rendezvous = "127.0.0.1:3478";
  std::string bind_host = "0.0.0.0";
  uint16_t bind_port = 0;
  std::string nickname;
  std::string profile_path;
  bool enable_lan = true;
  std::string peer_addr;
};

/** Разбирает host:port. @return false при ошибке формата. */
bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port);

/** Разбор argv после имени команды (listen / connect). */
NodeConfig parse_config(int argc, char** argv, int start_index);

/** Печатает справку по использованию. */
void print_usage();

}  // namespace nyx_node
