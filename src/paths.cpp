#include "nyx/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace nyx {

namespace {

std::string g_scoped_data_dir;
bool g_scoped = false;
std::mutex g_data_dir_mutex;

std::string join_path(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  if (dir.back() == '/' || dir.back() == '\\') return dir + name;
  return dir + '/' + name;
}

std::string base_data_root() {
#ifdef _WIN32
  if (const char* appdata = std::getenv("APPDATA")) {
    return join_path(appdata, "nyx");
  }
  return "nyx";
#else
  if (const char* home = std::getenv("HOME")) {
    return join_path(home, ".config/nyx");
  }
  return ".config/nyx";
#endif
}

}  // namespace

std::string data_root() { return base_data_root(); }

std::string data_dir() {
  std::lock_guard lock(g_data_dir_mutex);
  if (g_scoped && !g_scoped_data_dir.empty()) return g_scoped_data_dir;
  return base_data_root();
}

void set_account_data_dir(const std::string& account_dir) {
  std::lock_guard lock(g_data_dir_mutex);
  g_scoped_data_dir = account_dir;
  g_scoped = true;
}

void clear_account_data_dir() {
  std::lock_guard lock(g_data_dir_mutex);
  g_scoped_data_dir.clear();
  g_scoped = false;
}

std::string accounts_root() { return join_path(base_data_root(), "accounts"); }

std::string registry_path() { return join_path(base_data_root(), "registry.json"); }

std::string legacy_profile_path() { return join_path(base_data_root(), "profile.json"); }

std::string default_profile_path() {
  return join_path(data_dir(), kEncryptedProfileFilename);
}

std::string default_contacts_path() { return join_path(data_dir(), "contacts.json"); }

std::string default_downloads_dir() { return join_path(data_dir(), "downloads"); }

std::string default_logs_dir() { return join_path(data_dir(), "logs"); }

std::string default_log_file_path() { return join_path(default_logs_dir(), "nyx.log"); }

bool ensure_data_dir() {
  std::error_code ec;
  std::filesystem::create_directories(data_dir(), ec);
  return !ec;
}

}  // namespace nyx
