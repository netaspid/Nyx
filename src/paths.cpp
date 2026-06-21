#include "nyx/paths.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <cstdlib>
#include <filesystem>

namespace nyx {

namespace {

std::string join_path(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  if (dir.back() == '/' || dir.back() == '\\') return dir + name;
  return dir + '/' + name;
}

}  // namespace

std::string data_dir() {
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

std::string default_profile_path() { return join_path(data_dir(), "profile.json"); }

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
