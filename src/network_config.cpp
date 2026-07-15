#include "nyx/network_config.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <fstream>
#include <optional>
#include <sstream>

namespace nyx {

namespace {

std::string trim_copy(const std::string& s) {
  std::size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
                                s[start] == '\n')) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
                         s[end - 1] == '\n')) {
    --end;
  }
  return s.substr(start, end - start);
}

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else
      out += c;
  }
  return out;
}

std::optional<std::string> json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"') break;
    if (c == '\\' && i < json.size()) out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

bool parse_one_host_port(const std::string& part, RendezvousServer& out) {
  std::string host;
  uint16_t port = 0;
  if (!parse_host_port(part, host, port)) return false;
  out.host = host;
  out.port = port;
  return true;
}

}  // namespace

std::string NetworkConfig::config_path() { return data_dir() + "/network.json"; }

RendezvousServer NetworkConfig::primary_rendezvous() const {
  if (!rendezvous_servers.empty()) return rendezvous_servers.front();
  return RendezvousServer{"127.0.0.1", 3478, "local"};
}

std::string NetworkConfig::rendezvous_list_string() const {
  std::ostringstream ss;
  for (std::size_t i = 0; i < rendezvous_servers.size(); ++i) {
    if (i > 0) ss << ',';
    ss << rendezvous_servers[i].host << ':' << rendezvous_servers[i].port;
  }
  return ss.str();
}

bool NetworkConfig::parse_rendezvous_list(const std::string& csv, NetworkConfig& out) {
  out.rendezvous_servers.clear();
  const std::string trimmed = trim_copy(csv);
  if (trimmed.empty()) return false;
  std::string part;
  for (char c : trimmed) {
    if (c == ',' || c == ';') {
      part = trim_copy(part);
      if (!part.empty()) {
        RendezvousServer srv;
        if (!parse_one_host_port(part, srv)) return false;
        out.rendezvous_servers.push_back(std::move(srv));
        part.clear();
      }
      continue;
    }
    part.push_back(c);
  }
  part = trim_copy(part);
  if (!part.empty()) {
    RendezvousServer srv;
    if (!parse_one_host_port(part, srv)) return false;
    out.rendezvous_servers.push_back(srv);
  }
  return !out.rendezvous_servers.empty();
}

bool NetworkConfig::load() {
  rendezvous_servers.clear();
  std::ifstream file(config_path(), std::ios::binary);
  if (!file) {
    rendezvous_servers.push_back(RendezvousServer{"127.0.0.1", 3478, "local"});
    return true;
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();

  if (auto m = json_get_string(json, "mode")) {
    if (*m == "lan") mode = DiscoveryMode::LanOnly;
    else if (*m == "internet")
      mode = DiscoveryMode::Internet;
    else
      mode = DiscoveryMode::Auto;
  }

  if (auto stun = json_get_string(json, "stun_host")) stun_host = *stun;

  if (json.find("\"auto_start_owned_hub\":false") != std::string::npos) {
    auto_start_owned_hub = false;
  } else {
    auto_start_owned_hub = true;
  }

  std::size_t pos = 0;
  while ((pos = json.find("\"host\":\"", pos)) != std::string::npos) {
    const auto obj_start = json.rfind('{', pos);
    const auto obj_end = json.find('}', pos);
    if (obj_start == std::string::npos || obj_end == std::string::npos) break;
    const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
    RendezvousServer srv;
    if (auto h = json_get_string(obj, "host")) srv.host = *h;
    std::string port_str = "3478";
    const auto port_key = obj.find("\"port\":");
    if (port_key != std::string::npos) {
      try {
        srv.port = static_cast<uint16_t>(
            std::stoi(obj.substr(port_key + 7)));
      } catch (const std::exception&) {
        srv.port = 3478;
      }
    }
    if (auto label = json_get_string(obj, "label")) srv.label = *label;
    if (!srv.host.empty()) rendezvous_servers.push_back(std::move(srv));
    pos = obj_end;
  }

  if (rendezvous_servers.empty()) {
    rendezvous_servers.push_back(RendezvousServer{"127.0.0.1", 3478, "local"});
  }
  return true;
}

bool NetworkConfig::save() const {
  ensure_data_dir();
  std::ofstream file(config_path(), std::ios::binary | std::ios::trunc);
  if (!file) return false;

  const char* mode_str = "auto";
  if (mode == DiscoveryMode::LanOnly) mode_str = "lan";
  else if (mode == DiscoveryMode::Internet)
    mode_str = "internet";

  file << "{\"mode\":\"" << mode_str << "\",\"use_stun\":" << (use_stun ? "true" : "false")
       << ",\"stun_host\":\"" << json_escape(stun_host) << "\",\"stun_port\":" << stun_port
       << ",\"register_refresh_sec\":" << register_refresh_sec
       << ",\"auto_start_owned_hub\":" << (auto_start_owned_hub ? "true" : "false")
       << ",\"rendezvous\":[";
  for (std::size_t i = 0; i < rendezvous_servers.size(); ++i) {
    if (i > 0) file << ',';
    const auto& r = rendezvous_servers[i];
    file << "{\"host\":\"" << json_escape(r.host) << "\",\"port\":" << r.port
         << ",\"label\":\"" << json_escape(r.label) << "\"}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

}  // namespace nyx
