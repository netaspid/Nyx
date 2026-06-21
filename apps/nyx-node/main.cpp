#include "cli.hpp"
#include "group_session.hpp"
#include "node_session.hpp"

#include "nyx/console.hpp"
#include "nyx/log.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  nyx::setup_console_utf8();
  nyx::log_init();

  if (argc < 2) {
    nyx_node::print_usage();
    return 0;
  }

  const std::string command = argv[1];

  if (command == "listen") {
    try {
      const auto config = nyx_node::parse_config(argc, argv, 2);
      return nyx_node::run_listen(config);
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << '\n';
      return 1;
    }
  }

  if (command == "connect") {
    std::string token;
    bool has_peer = false;
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--token" && i + 1 < argc) {
        token = argv[++i];
      } else if (std::string(argv[i]) == "--peer" && i + 1 < argc) {
        has_peer = true;
      }
    }
    try {
      const auto config = nyx_node::parse_config(argc, argv, 2);
      if (has_peer || !config.peer_addr.empty()) {
        return nyx_node::run_connect_peer(config);
      }
      if (token.empty()) {
        std::cerr << "укажите --token <hex> или --peer host:port\n";
        nyx_node::print_usage();
        return 1;
      }
      return nyx_node::run_connect(token, config);
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << '\n';
      return 1;
    }
  }

  if (command == "browse") {
    int timeout_ms = 3000;
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--timeout" && i + 1 < argc) {
        timeout_ms = std::stoi(argv[++i]);
      }
    }
    return nyx_node::run_browse(timeout_ms);
  }

  if (command == "group") {
    if (argc < 3) {
      std::cerr << "group create <имя> | group hub --group <id> | group join --token <hex>\n";
      return 1;
    }
    const std::string sub = argv[2];
    try {
      const auto config = nyx_node::parse_config(argc, argv, 3);
      if (sub == "create") {
        if (argc < 4) {
          std::cerr << "group create <имя>\n";
          return 1;
        }
        return nyx_node::run_group_create(argv[3], config);
      }
      if (sub == "hub") {
        std::string group_id;
        for (int i = 3; i < argc; ++i) {
          if (std::string(argv[i]) == "--group" && i + 1 < argc) {
            group_id = argv[++i];
          }
        }
        if (group_id.empty()) {
          std::cerr << "group hub --group <64-hex group_id>\n";
          return 1;
        }
        return nyx_node::run_group_hub(group_id, config);
      }
      if (sub == "join") {
        std::string token;
        for (int i = 3; i < argc; ++i) {
          if (std::string(argv[i]) == "--token" && i + 1 < argc) {
            token = argv[++i];
          }
        }
        if (token.empty()) {
          std::cerr << "group join --token <64-hex invite>\n";
          return 1;
        }
        return nyx_node::run_group_join(token, config);
      }
      std::cerr << "неизвестная подкоманда group: " << sub << '\n';
      return 1;
    } catch (const std::exception& ex) {
      std::cerr << ex.what() << '\n';
      return 1;
    }
  }

  std::cerr << "неизвестная команда: " << command << '\n';
  nyx_node::print_usage();
  return 1;
}
