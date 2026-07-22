#include "setup_util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace nyx_setup {

namespace {

std::string join_path(const std::string& dir, const char* name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + '/' + name;
}

std::string home_dir() {
  if (const char* home = std::getenv("HOME")) return home;
  return ".";
}

std::string xdg_data_home() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME")) return xdg;
  return join_path(home_dir(), ".local/share");
}

bool path_exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool process_exe_matches_dir(pid_t pid, const std::string& install_dir) {
  const std::string exe_link = "/proc/" + std::to_string(pid) + "/exe";
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(exe_link, ec);
  if (ec) return true;
  const auto canon_install = std::filesystem::weakly_canonical(install_dir, ec);
  const auto canon_target = std::filesystem::weakly_canonical(target, ec);
  if (ec) return true;
  const std::string prefix = canon_install.string();
  const std::string path = canon_target.string();
  if (path.size() < prefix.size()) return false;
  if (path.compare(0, prefix.size(), prefix) != 0) return false;
  return path.size() == prefix.size() || path[prefix.size()] == '/';
}

bool terminate_by_name_in_dir(const char* exe_name, const std::string& install_dir) {
  bool stopped = false;
  const std::filesystem::path proc_dir("/proc");
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(proc_dir, ec)) {
    if (!entry.is_directory()) continue;
    const auto name = entry.path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
    const pid_t pid = static_cast<pid_t>(std::stoi(name));
    const auto cmdline_path = entry.path() / "cmdline";
    std::ifstream cmdline_file(cmdline_path, std::ios::binary);
    if (!cmdline_file) continue;
    std::string cmd;
    std::getline(cmdline_file, cmd, '\0');
    const auto slash = cmd.find_last_of('/');
    const std::string base = slash == std::string::npos ? cmd : cmd.substr(slash + 1);
    if (base != exe_name) continue;
    if (!process_exe_matches_dir(pid, install_dir)) continue;
    kill(pid, SIGTERM);
    stopped = true;
  }
  if (stopped) sleep(1);
  return stopped;
}

}  // namespace

bool stop_nyx_for_install(const std::string& install_dir, std::string* err) {
  (void)err;
  const char* names[] = {"nyx-app", "nyx-node", "nyx-rendezvous"};
  for (const char* name : names) terminate_by_name_in_dir(name, install_dir);
  return true;
}

std::string default_install_dir() { return join_path(xdg_data_home(), "Nyx"); }

std::string data_root() { return join_path(home_dir(), ".config/nyx"); }

bool create_desktop_entry(const std::string& install_dir) {
  const std::string apps_dir = join_path(xdg_data_home(), "applications");
  std::error_code ec;
  std::filesystem::create_directories(apps_dir, ec);

  const std::string app_exe = join_path(install_dir, "nyx-app-wrapper.sh");
  const std::string icon = join_path(install_dir, "nyx-icon.svg");
  const std::string desktop_path = join_path(apps_dir, "nyx.desktop");

  std::ostringstream out;
  out << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=Nyx\n"
      << "Comment=P2P messenger\n"
      << "Exec=" << app_exe << "\n"
      << "Path=" << install_dir << "\n"
      << "Icon=" << icon << "\n"
      << "Terminal=false\n"
      << "Categories=Network;InstantMessaging;\n";

  std::ofstream file(desktop_path, std::ios::trunc);
  if (!file) return false;
  file << out.str();
  return static_cast<bool>(file);
}

bool remove_desktop_entry() {
  const std::string desktop_path = join_path(join_path(xdg_data_home(), "applications"), "nyx.desktop");
  std::error_code ec;
  std::filesystem::remove(desktop_path, ec);
  return true;
}

bool register_install_manifest(const std::string& install_dir) {
  const std::string manifest_dir = join_path(xdg_data_home(), "nyx");
  std::error_code ec;
  std::filesystem::create_directories(manifest_dir, ec);
  std::ofstream out(join_path(manifest_dir, "install-location"), std::ios::trunc);
  if (!out) return false;
  out << install_dir << '\n';
  return static_cast<bool>(out);
}

bool launch_app(const std::string& exe_path) {
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    execl(exe_path.c_str(), exe_path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return true;
}

bool verify_installation(const std::string& install_dir, std::string* err) {
  const std::string app = join_path(install_dir, "nyx-app");
  if (!path_exists(app)) {
    if (err) *err = "nyx-app not found after install";
    return false;
  }

  const char* required[] = {
      "libQt6Core.so.6",
      "libQt6Gui.so.6",
      "libQt6Qml.so.6",
      "libQt6Quick.so.6",
      "libQt6Network.so.6",
      "libQt6Multimedia.so.6",
      "libQt6XcbQpa.so.6",
      "libxcb-cursor.so.0",
      "libicui18n.so.56",
      "libicuuc.so.56",
      "libicudata.so.56",
      "platforms/libqxcb.so",
      "xcbglintegrations/libqxcb-glx-integration.so",
      "xcbglintegrations/libqxcb-egl-integration.so",
  };

  for (const char* rel : required) {
    const std::string full = join_path(install_dir, rel);
    if (!path_exists(full)) {
      if (err) *err = std::string("Missing component: ") + rel;
      return false;
    }
  }
  return true;
}

bool repair_installation(const std::vector<std::uint8_t>& blob, const std::string& install_dir,
                         std::string* err) {
  std::vector<PayloadFile> files;
  if (!parse_payload(blob, files)) {
    if (err) *err = "Invalid installer payload";
    return false;
  }
  for (const auto& f : files) {
    if (f.data.empty()) continue;
    const std::filesystem::path full = std::filesystem::path(install_dir) / f.relative_path;
    bool needs_copy = true;
    std::error_code ec;
    if (std::filesystem::exists(full, ec)) {
      const auto existing = std::filesystem::file_size(full, ec);
      if (!ec && existing == f.data.size()) needs_copy = false;
    }
    if (!needs_copy) continue;
    std::string narrow_err;
    if (!extract_payload(blob, install_dir, nullptr, &narrow_err)) {
      if (err) *err = narrow_err;
      return false;
    }
    break;
  }
  return verify_installation(install_dir, err);
}

}  // namespace nyx_setup
