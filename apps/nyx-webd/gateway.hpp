#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nyx_web {

struct GatewayConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8787;
  std::string data_root;
  std::string static_dir;
  std::string admin_token;
  int max_sessions = 16;
  std::string self_exe;
};

class Gateway {
public:
  explicit Gateway(GatewayConfig cfg);
  ~Gateway();

  void run();
  void request_stop();

private:
  struct Session;

  GatewayConfig cfg_;
  std::atomic<bool> stop_ {false};
  std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;

  std::shared_ptr<Session> create_session();
  std::shared_ptr<Session> find_by_token(const std::string& token);
  void destroy_session(const std::string& token);
};

int run_worker_mode(const std::string& data_root, uint16_t port);

} // namespace nyx_web
