#include "nyx/log.hpp"
#include "nyx/rendezvous_server.hpp"
#include "nyx/util.hpp"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#endif

namespace {

volatile std::sig_atomic_t g_running = 1;

void on_signal(int) { g_running = 0; }

bool parse_args(int argc, char** argv, nyx::RendezvousServerConfig& cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--bind=", 0) == 0) {
      std::string host;
      uint16_t port = 0;
      if (!nyx::parse_host_port(arg.substr(7), host, port)) return false;
      cfg.bind_host = host;
      cfg.bind_port = port;
    } else if (arg == "--help" || arg == "-h") {
      return false;
    } else if (arg.rfind("--rate-limit=", 0) == 0) {
      cfg.rate_limit_per_minute =
          static_cast<std::uint32_t>(std::stoul(arg.substr(13)));
    }
  }
  return true;
}

void print_usage() {
  std::cout << "nyx-rendezvous — UDP bootstrap для invite token\n\n"
            << "Usage:\n"
            << "  nyx-rendezvous [--bind=0.0.0.0:3478] [--rate-limit=120]\n\n"
            << "Откройте UDP порт на VDS (firewall + security group).\n"
            << "См. docs/DEPLOY_RENDEZVOUS.md\n";
}

std::string client_ip(const sockaddr_in& from) {
  char buf[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &from.sin_addr, buf, sizeof(buf));
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  nyx::log_init();
  nyx::RendezvousServerConfig cfg;
  if (!parse_args(argc, argv, cfg)) {
    print_usage();
    return argc > 1 ? 1 : 0;
  }

#ifndef _WIN32
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#endif

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    std::cerr << "socket failed\n";
    return 1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg.bind_port);
  inet_pton(AF_INET, cfg.bind_host.c_str(), &addr.sin_addr);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind failed on " << cfg.bind_host << ':' << cfg.bind_port << '\n';
    return 1;
  }

  nyx::RendezvousRegistry registry(cfg);
  nyx::log_info("rendezvous listening on " + cfg.bind_host + ':' +
                  std::to_string(cfg.bind_port));

  uint8_t buf[2048];
  while (g_running) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    const int sel = select(static_cast<int>(sock) + 1, &fds, nullptr, nullptr, &tv);
    if (sel <= 0) continue;

    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) continue;

    nyx::ByteBuffer in(buf, buf + n);
    if (auto reply = registry.handle_datagram(client_ip(from), in)) {
      sendto(sock, reinterpret_cast<const char*>(reply->data()),
             static_cast<int>(reply->size()), 0,
             reinterpret_cast<sockaddr*>(&from), from_len);
    }
  }

  nyx::log_info("rendezvous shutdown");
#ifdef _WIN32
  closesocket(sock);
  WSACleanup();
#else
  close(sock);
#endif
  return 0;
}
