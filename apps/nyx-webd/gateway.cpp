#include "gateway.hpp"
#include "json_util.hpp"
#include "worker_engine.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace nyx_web {
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

std::string random_token() {
  static thread_local std::mt19937_64 rng {std::random_device {}()};
  std::ostringstream ss;
  for (int i = 0; i < 4; ++i)
    ss << std::hex << rng();
  return ss.str();
}

uint16_t find_free_port() {
  net::io_context ioc;
  tcp::acceptor acc(ioc, tcp::endpoint(tcp::v4(), 0));
  return acc.local_endpoint().port();
}

std::string mime_for(const std::string& path) {
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".html")
    return "text/html; charset=utf-8";
  if (path.size() >= 3 && path.substr(path.size() - 3) == ".js")
    return "application/javascript; charset=utf-8";
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".css")
    return "text/css; charset=utf-8";
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".svg")
    return "image/svg+xml";
  if (path.size() >= 5 && path.substr(path.size() - 5) == ".json")
    return "application/json";
  return "application/octet-stream";
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

} // namespace

struct WorkerProc {
  uint16_t port = 0;
#if !defined(_WIN32)
  pid_t pid = -1;
#endif
};

static WorkerProc spawn_worker(const GatewayConfig& cfg, const std::string& instance_root) {
  WorkerProc wp;
  wp.port = find_free_port();
  std::filesystem::create_directories(instance_root);
#if defined(_WIN32)
  std::string cmd = "\"" + cfg.self_exe + "\" --worker --listen 127.0.0.1:" +
                    std::to_string(wp.port) + " --data-root \"" + instance_root + "\"";
  auto* proc = _popen(cmd.c_str(), "r");
  (void)proc;
#else
  pid_t pid = fork();
  if (pid == 0) {
    const std::string listen = "127.0.0.1:" + std::to_string(wp.port);
    execl(cfg.self_exe.c_str(),
         cfg.self_exe.c_str(),
         "--worker",
         "--listen",
         listen.c_str(),
         "--data-root",
         instance_root.c_str(),
         static_cast<char*>(nullptr));
    _exit(127);
  }
  wp.pid = pid;
  // Give worker a moment to bind.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
#endif
  return wp;
}

struct Gateway::Session {
  std::string id;
  std::string token;
  std::string instance_root;
  WorkerProc worker;
};

Gateway::Gateway(GatewayConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.data_root.empty()) {
    const char* home = std::getenv("HOME");
    cfg_.data_root = home ? std::string(home) + "/.config/nyx-web" : ".config/nyx-web";
  }
  std::filesystem::create_directories(cfg_.data_root);
}

Gateway::~Gateway() {
  request_stop();
#if !defined(_WIN32)
  std::lock_guard lock(sessions_mutex_);
  for (auto& [_, s] : sessions_) {
    if (s && s->worker.pid > 0)
      kill(s->worker.pid, SIGTERM);
  }
#endif
}

void Gateway::request_stop() {
  stop_.store(true);
}

std::shared_ptr<Gateway::Session> Gateway::create_session() {
  std::lock_guard lock(sessions_mutex_);
  if (static_cast<int>(sessions_.size()) >= cfg_.max_sessions)
    return nullptr;
  auto s = std::make_shared<Session>();
  s->id = random_token();
  s->token = random_token();
  s->instance_root = cfg_.data_root + "/instances/" + s->id;
  s->worker = spawn_worker(cfg_, s->instance_root);
  sessions_[s->token] = s;
  return s;
}

std::shared_ptr<Gateway::Session> Gateway::find_by_token(const std::string& token) {
  std::lock_guard lock(sessions_mutex_);
  auto it = sessions_.find(token);
  if (it == sessions_.end())
    return nullptr;
  return it->second;
}

void Gateway::destroy_session(const std::string& token) {
  std::shared_ptr<Session> s;
  {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end())
      return;
    s = it->second;
    sessions_.erase(it);
  }
#if !defined(_WIN32)
  if (s && s->worker.pid > 0)
    kill(s->worker.pid, SIGTERM);
#endif
}

static std::string worker_rpc(uint16_t port, const std::string& body) {
  try {
    net::io_context ioc;
    tcp::socket sock(ioc);
    sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), port));
    const std::string req = "POST /rpc HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                            "Content-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    net::write(sock, net::buffer(req));
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(sock, buffer, res);
    return res.body();
  } catch (const std::exception& ex) {
    return std::string("{\"type\":\"rpc_result\",\"ok\":false,\"error\":") + json_str(ex.what()) +
           "}";
  }
}

void Gateway::run() {
  net::io_context ioc {1};
  tcp::acceptor acceptor {ioc, {net::ip::make_address(cfg_.listen_host), cfg_.listen_port}};
  std::cout << "nyx-webd listening on http://" << cfg_.listen_host << ":" << cfg_.listen_port
            << "\n";

  while (!stop_.load()) {
    tcp::socket socket {ioc};
    boost::system::error_code ec;
    acceptor.accept(socket, ec);
    if (ec)
      continue;
    std::thread([this, sock = std::move(socket)]() mutable {
      try {
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(sock, buffer, req);

        auto send_json = [&](http::status st, const std::string& body) {
          http::response<http::string_body> res {st, req.version()};
          res.set(http::field::server, "nyx-webd");
          res.set(http::field::content_type, "application/json; charset=utf-8");
          res.set(http::field::access_control_allow_origin, "*");
          res.set(http::field::access_control_allow_headers, "Authorization, Content-Type");
          res.keep_alive(false);
          res.body() = body;
          res.prepare_payload();
          http::write(sock, res);
        };

        if (req.method() == http::verb::options) {
          http::response<http::empty_body> res {http::status::no_content, req.version()};
          res.set(http::field::access_control_allow_origin, "*");
          res.set(http::field::access_control_allow_methods, "GET, POST, DELETE, OPTIONS");
          res.set(http::field::access_control_allow_headers, "Authorization, Content-Type");
          res.prepare_payload();
          http::write(sock, res);
          return;
        }

        const std::string target(req.target());
        if (target == "/api/v1/health") {
          send_json(http::status::ok, "{\"ok\":true,\"service\":\"nyx-webd\"}");
          return;
        }

        if (target == "/api/v1/session" && req.method() == http::verb::post) {
          if (!cfg_.admin_token.empty()) {
            auto auth = req[http::field::authorization];
            const std::string expect = "Bearer " + cfg_.admin_token;
            if (auth != expect) {
              send_json(http::status::unauthorized, "{\"error\":\"unauthorized\"}");
              return;
            }
          }
          auto s = create_session();
          if (!s) {
            send_json(http::status::service_unavailable, "{\"error\":\"max sessions\"}");
            return;
          }
          send_json(http::status::ok,
                    "{\"sessionId\":" + json_str(s->id) + ",\"token\":" + json_str(s->token) +
                        ",\"workerPort\":" + std::to_string(s->worker.port) + "}");
          return;
        }

        if (target.rfind("/api/v1/session", 0) == 0 && req.method() == http::verb::delete_) {
          auto auth = std::string(req[http::field::authorization]);
          std::string token;
          if (auth.rfind("Bearer ", 0) == 0)
            token = auth.substr(7);
          destroy_session(token);
          send_json(http::status::ok, "{\"ok\":true}");
          return;
        }

        if (target == "/api/v1/rpc" && req.method() == http::verb::post) {
          auto auth = std::string(req[http::field::authorization]);
          std::string token;
          if (auth.rfind("Bearer ", 0) == 0)
            token = auth.substr(7);
          auto s = find_by_token(token);
          if (!s) {
            send_json(http::status::unauthorized, "{\"error\":\"bad session\"}");
            return;
          }
          send_json(http::status::ok, worker_rpc(s->worker.port, req.body()));
          return;
        }

        auto query_param = [](const std::string& t, const char* key) -> std::string {
          const std::string pat = std::string(key) + "=";
          auto q = t.find('?');
          if (q == std::string::npos)
            return {};
          auto pos = t.find(pat, q);
          if (pos == std::string::npos)
            return {};
          pos += pat.size();
          auto end = t.find('&', pos);
          if (end == std::string::npos)
            end = t.size();
          return t.substr(pos, end - pos);
        };

        // GET /api/v1/files/blob?token=...&hash=...
        if (target.rfind("/api/v1/files/blob", 0) == 0 && req.method() == http::verb::get) {
          std::string token = query_param(target, "token");
          if (token.empty()) {
            auto auth = std::string(req[http::field::authorization]);
            if (auth.rfind("Bearer ", 0) == 0)
              token = auth.substr(7);
          }
          const std::string hash = query_param(target, "hash");
          auto s = find_by_token(token);
          if (!s || hash.empty()) {
            send_json(http::status::unauthorized, "{\"error\":\"bad session or hash\"}");
            return;
          }
          const std::string rpc =
              "{\"id\":\"blob\",\"op\":\"fileBlobPath\",\"args\":{\"hash\":" + json_str(hash) + "}}";
          const std::string reply = worker_rpc(s->worker.port, rpc);
          auto path = json_get_string(reply, "result");
          if (!path || path->empty()) {
            send_json(http::status::not_found, reply);
            return;
          }
          std::string body;
          if (!read_file(*path, body)) {
            send_json(http::status::not_found, "{\"error\":\"file missing on disk\"}");
            return;
          }
          http::response<http::string_body> res {http::status::ok, req.version()};
          res.set(http::field::content_type, "application/octet-stream");
          res.set(http::field::content_disposition,
                  "attachment; filename=\"" + std::filesystem::path(*path).filename().string() + "\"");
          res.body() = std::move(body);
          res.prepare_payload();
          http::write(sock, res);
          return;
        }

        // POST /api/v1/files/upload?token=...&dest=...  (raw body)
        if (target.rfind("/api/v1/files/upload", 0) == 0 && req.method() == http::verb::post) {
          std::string token = query_param(target, "token");
          if (token.empty()) {
            auto auth = std::string(req[http::field::authorization]);
            if (auth.rfind("Bearer ", 0) == 0)
              token = auth.substr(7);
          }
          std::string dest = query_param(target, "dest");
          auto s = find_by_token(token);
          if (!s || dest.empty()) {
            send_json(http::status::bad_request, "{\"error\":\"token and dest required\"}");
            return;
          }
          // Percent-decode minimal (%2F → /)
          {
            std::string d;
            for (std::size_t i = 0; i < dest.size(); ++i) {
              if (dest[i] == '%' && i + 2 < dest.size()) {
                auto hex = [](char c) -> int {
                  if (c >= '0' && c <= '9')
                    return c - '0';
                  if (c >= 'a' && c <= 'f')
                    return 10 + c - 'a';
                  if (c >= 'A' && c <= 'F')
                    return 10 + c - 'A';
                  return -1;
                };
                int hi = hex(dest[i + 1]), lo = hex(dest[i + 2]);
                if (hi >= 0 && lo >= 0) {
                  d.push_back(static_cast<char>((hi << 4) | lo));
                  i += 2;
                  continue;
                }
              }
              d.push_back(dest[i] == '+' ? ' ' : dest[i]);
            }
            dest = std::move(d);
          }
          std::filesystem::create_directories(std::filesystem::path(dest).parent_path());
          {
            std::ofstream out(dest, std::ios::binary);
            if (!out) {
              send_json(http::status::internal_server_error, "{\"error\":\"cannot write dest\"}");
              return;
            }
            out.write(req.body().data(), static_cast<std::streamsize>(req.body().size()));
          }
          send_json(http::status::ok,
                    "{\"ok\":true,\"dest\":" + json_str(dest) + ",\"bytes\":" +
                        std::to_string(req.body().size()) + "}");
          return;
        }

        // WebSocket upgrade to worker events+rpc multiplex: /api/v1/ws?token=
        if (websocket::is_upgrade(req)) {
          std::string token;
          auto q = target.find("token=");
          if (q != std::string::npos)
            token = target.substr(q + 6);
          auto amp = token.find('&');
          if (amp != std::string::npos)
            token = token.substr(0, amp);
          auto s = find_by_token(token);
          if (!s) {
            send_json(http::status::unauthorized, "{\"error\":\"bad session\"}");
            return;
          }
          websocket::stream<tcp::socket> ws {std::move(sock)};
          ws.accept(req);

          // Bridge: open websocket to worker /ws
          net::io_context ioc2;
          tcp::socket wsock(ioc2);
          wsock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), s->worker.port));
          websocket::stream<tcp::socket> wsw {std::move(wsock)};
          http::request<http::empty_body> wre {http::verb::get, "/ws", 11};
          wre.set(http::field::host, "127.0.0.1");
          wre.set(http::field::upgrade, "websocket");
          wre.set(http::field::connection, "upgrade");
          wsw.handshake("127.0.0.1", "/ws");

          std::atomic<bool> done {false};
          std::thread up([&] {
            try {
              while (!done.load()) {
                beast::flat_buffer b;
                wsw.read(b);
                ws.text(wsw.got_text());
                ws.write(b.data());
              }
            } catch (...) {
              done.store(true);
            }
          });
          try {
            while (!done.load()) {
              beast::flat_buffer b;
              ws.read(b);
              wsw.text(ws.got_text());
              wsw.write(b.data());
            }
          } catch (...) {
            done.store(true);
          }
          beast::error_code ignored;
          wsw.close(websocket::close_code::normal, ignored);
          ws.close(websocket::close_code::normal, ignored);
          up.join();
          return;
        }

        // Static files
        std::string path = target;
        if (path == "/" || path.empty())
          path = "/index.html";
        if (path.find("..") != std::string::npos) {
          send_json(http::status::bad_request, "{\"error\":\"bad path\"}");
          return;
        }
        const std::string file = cfg_.static_dir + path;
        std::string body;
        if (!cfg_.static_dir.empty() && read_file(file, body)) {
          http::response<http::string_body> res {http::status::ok, req.version()};
          res.set(http::field::content_type, mime_for(path));
          res.body() = std::move(body);
          res.prepare_payload();
          http::write(sock, res);
          return;
        }
        // Built-in fallback shell if SPA not built yet
        if (path == "/index.html") {
          http::response<http::string_body> res {http::status::ok, req.version()};
          res.set(http::field::content_type, "text/html; charset=utf-8");
          res.body() = R"(<!doctype html><html><head><meta charset=utf-8><title>Nyx Web</title>
<link rel=stylesheet href=/assets/index.css></head>
<body><div id=app></div><script type=module src=/assets/index.js></script></body></html>)";
          res.prepare_payload();
          http::write(sock, res);
          return;
        }
        send_json(http::status::not_found, "{\"error\":\"not found\"}");
      } catch (const std::exception& ex) {
        std::cerr << "conn error: " << ex.what() << "\n";
      }
    }).detach();
  }
}

int run_worker_mode(const std::string& data_root, uint16_t port) {
  WorkerEngine engine(data_root);
  net::io_context ioc {1};
  tcp::acceptor acceptor {ioc, {net::ip::make_address("127.0.0.1"), port}};
  std::cout << "nyx-web-worker on 127.0.0.1:" << port << " root=" << data_root << std::endl;

  struct Client {
    std::shared_ptr<websocket::stream<tcp::socket>> ws;
    std::mutex mu;
  };
  auto clients = std::make_shared<std::vector<std::shared_ptr<Client>>>();
  std::mutex clients_mu;

  engine.set_event_sink([clients, &clients_mu](const std::string& ev) {
    std::vector<std::shared_ptr<Client>> snap;
    {
      std::lock_guard lock(clients_mu);
      snap = *clients;
    }
    for (auto& c : snap) {
      try {
        std::lock_guard lock(c->mu);
        c->ws->text(true);
        c->ws->write(net::buffer(ev));
      } catch (...) {
      }
    }
  });

  for (;;) {
    tcp::socket socket {ioc};
    acceptor.accept(socket);
    std::thread([sock = std::move(socket), &engine, clients, &clients_mu]() mutable {
      try {
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(sock, buffer, req);
        if (websocket::is_upgrade(req)) {
          auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(sock));
          ws->accept(req);
          auto client = std::make_shared<Client>();
          client->ws = ws;
          {
            std::lock_guard lock(clients_mu);
            clients->push_back(client);
          }
          for (;;) {
            beast::flat_buffer b;
            ws->read(b);
            const std::string msg = beast::buffers_to_string(b.data());
            const std::string reply = engine.handle_rpc(msg);
            std::lock_guard lock(client->mu);
            ws->text(true);
            ws->write(net::buffer(reply));
          }
        }
        if (std::string(req.target()) == "/rpc" && req.method() == http::verb::post) {
          http::response<http::string_body> res {http::status::ok, req.version()};
          res.set(http::field::content_type, "application/json");
          res.body() = engine.handle_rpc(req.body());
          res.prepare_payload();
          http::write(sock, res);
          return;
        }
        if (std::string(req.target()) == "/health") {
          http::response<http::string_body> res {http::status::ok, req.version()};
          res.body() = "{\"ok\":true}";
          res.prepare_payload();
          http::write(sock, res);
        }
      } catch (...) {
      }
    }).detach();
  }
  return 0;
}

} // namespace nyx_web
