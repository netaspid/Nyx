#include "cli.hpp"

#include "nyx/util.hpp"

#include <iostream>
#include <stdexcept>

namespace nyx_node {

bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port) {
  return nyx::parse_host_port(addr, host, port);
}

NodeConfig parse_config(int argc, char** argv, int start_index) {
  NodeConfig config;
  for (int i = start_index; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--rendezvous" && i + 1 < argc) {
      config.rendezvous = argv[++i];
    } else if (arg == "--bind" && i + 1 < argc) {
      std::string bind = argv[++i];
      if (!parse_host_port(bind, config.bind_host, config.bind_port)) {
        throw std::runtime_error("неверный формат --bind, нужен host:port");
      }
    } else if (arg == "--nickname" && i + 1 < argc) {
      config.nickname = argv[++i];
    } else if (arg == "--profile" && i + 1 < argc) {
      config.profile_path = argv[++i];
    } else if (arg == "--no-lan") {
      config.enable_lan = false;
    } else if (arg == "--lan") {
      config.enable_lan = true;
    } else if (arg == "--peer" && i + 1 < argc) {
      config.peer_addr = argv[++i];
    }
  }
  return config;
}

void print_usage() {
  std::cout
      << "Nyx node — P2P узел\n\n"
      << "  nyx-node listen [--rendezvous host:port[,host2:port]] [--bind 0.0.0.0:0]\n"
      << "                   [--nickname NAME] [--profile PATH] [--no-lan]\n"
      << "  nyx-node connect --token <hex> | --peer HOST:PORT\n"
      << "                    [--rendezvous host:port[,host2:port]]\n"
      << "                    [--nickname NAME] [--profile PATH]\n"
      << "  nyx-node browse [--timeout MS]\n\n"
      << "  nyx-node group create <имя> [--profile PATH]\n"
      << "  nyx-node group hub --group <group_id_hex> [--rendezvous ...]\n"
      << "  nyx-node group join --token <invite_hex> [--rendezvous ...]\n\n"
      << "  --no-lan  не публиковать узел в LAN (по умолчанию mDNS включён)\n"
      << "  Rendezvous: UDP bootstrap для интернет-связи. См. docs/DEPLOY_RENDEZVOUS.md\n\n"
      << "В чате: текст + Enter. Команды: /help /who /status /history /quit\n";
}

}  // namespace nyx_node
