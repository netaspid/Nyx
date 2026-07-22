#include "setup_util.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool ask_yes_no(const char* prompt) {
  std::cout << prompt << " [y/N] ";
  std::string line;
  if (!std::getline(std::cin, line)) return false;
  return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

bool remove_dir_recursive(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return !ec;
}

std::string read_install_dir() {
  char self[4096];
  const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (n <= 0) return {};
  self[n] = '\0';
  const std::filesystem::path path(self);
  return path.parent_path().string();
}

}  // namespace

int main() {
  const std::string install_dir = read_install_dir();
  if (install_dir.empty()) {
    std::cerr << "Cannot determine install directory.\n";
    return 1;
  }

  std::cout << "Remove Nyx from " << install_dir << "?\n";
  if (!ask_yes_no("Uninstall")) return 0;

  nyx_setup::stop_nyx_for_install(install_dir);
  nyx_setup::remove_desktop_entry();

  if (ask_yes_no("Delete local Nyx data (accounts, chats, keys)?")) {
    remove_dir_recursive(nyx_setup::data_root());
  }

  if (!remove_dir_recursive(install_dir)) {
    std::cerr << "Failed to remove " << install_dir << '\n';
    return 1;
  }

  std::cout << "Nyx has been removed.\n";
  return 0;
}
