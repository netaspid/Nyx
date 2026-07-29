#pragma once

#include <cstdint>
#include <string>

namespace nyx_node {

struct NodeConfig {
  std::string rendezvous = "127.0.0.1:3478";
  std::string bind_host = "0.0.0.0";
  uint16_t bind_port = 0;
  std::string nickname;
  std::string profile_path;
  bool enable_lan = true;
  std::string peer_addr;
};

bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port);

NodeConfig parse_config(int argc, char** argv, int start_index);

void print_usage();

}
