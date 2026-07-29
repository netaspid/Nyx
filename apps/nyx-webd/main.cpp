#include "gateway.hpp"
#include "json_util.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <limits.h>
#include <unistd.h>
#endif

namespace {

std::string detect_self_exe(const char* argv0) {
#if !defined(_WIN32)
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = 0;
    return std::string(buf);
  }
#endif
  return argv0 ? argv0 : "nyx-webd";
}

void usage() {
  std::cerr
      << "nyx-webd --listen HOST:PORT [--data-root PATH] [--static-dir PATH] [--admin-token TOKEN]\n"
      << "nyx-webd --worker --listen HOST:PORT --data-root PATH\n";
}

bool parse_host_port(const std::string& s, std::string& host, uint16_t& port) {
  auto pos = s.rfind(':');
  if (pos == std::string::npos)
    return false;
  host = s.substr(0, pos);
  port = static_cast<uint16_t>(std::stoi(s.substr(pos + 1)));
  return !host.empty() && port != 0;
}

} // namespace

int main(int argc, char** argv) {
  bool worker = false;
  std::string listen = "127.0.0.1:8787";
  std::string data_root;
  std::string static_dir;
  std::string admin_token;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--worker")
      worker = true;
    else if (a == "--listen")
      listen = need("--listen");
    else if (a == "--data-root")
      data_root = need("--data-root");
    else if (a == "--static-dir")
      static_dir = need("--static-dir");
    else if (a == "--admin-token")
      admin_token = need("--admin-token");
    else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  std::string host;
  uint16_t port = 0;
  if (!parse_host_port(listen, host, port)) {
    std::cerr << "bad --listen\n";
    return 2;
  }

  if (worker) {
    if (data_root.empty()) {
      std::cerr << "--worker requires --data-root\n";
      return 2;
    }
    return nyx_web::run_worker_mode(data_root, port);
  }

  nyx_web::GatewayConfig cfg;
  cfg.listen_host = host;
  cfg.listen_port = port;
  cfg.data_root = data_root;
  cfg.static_dir = static_dir;
  cfg.admin_token = admin_token;
  cfg.self_exe = detect_self_exe(argv[0]);

  // Default static dir next to build tree if present.
  if (cfg.static_dir.empty()) {
    const std::string cand = "apps/nyx-web/dist";
    cfg.static_dir = cand;
  }

  nyx_web::Gateway gw(std::move(cfg));
  gw.run();
  return 0;
}
