#include "../nyx-setup/payload.hpp"
#include "setup_util.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_progress(int pct, const std::string& status) {
  std::cerr << '\r' << pct << "% " << status << std::flush;
  if (pct >= 100) std::cerr << '\n';
}

bool ask_yes_no(const char* prompt) {
  std::cout << prompt << " [y/N] ";
  std::string line;
  if (!std::getline(std::cin, line)) return false;
  return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::uint8_t> payload;
  if (!nyx_setup::read_self_payload(payload)) {
    std::cerr << "Not a Nyx installer. Build with: cmake --build build -j\n";
    return 1;
  }

  std::string install_dir = nyx_setup::default_install_dir();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dir" && i + 1 < argc) {
      install_dir = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: NyxSetup [--dir PATH]\n";
      return 0;
    }
  }

  std::cout << "Nyx installer\n";
  std::cout << "Install directory: " << install_dir << '\n';
  if (!ask_yes_no("Continue?")) return 0;

  std::string err;
  std::cerr << "Stopping Nyx...\n";
  nyx_setup::stop_nyx_for_install(install_dir, &err);

  std::cerr << "Installing...\n";
  if (!nyx_setup::extract_payload(payload, install_dir, print_progress, &err)) {
    std::cerr << "Install failed: " << err << '\n';
    return 1;
  }

  if (!nyx_setup::verify_installation(install_dir, &err)) {
    std::cerr << "Verifying, repairing...\n";
    if (!nyx_setup::repair_installation(payload, install_dir, &err) ||
        !nyx_setup::verify_installation(install_dir, &err)) {
      std::cerr << "Install verification failed: " << err << '\n';
      return 1;
    }
  }

  nyx_setup::create_desktop_entry(install_dir);
  nyx_setup::register_install_manifest(install_dir);

  std::cout << "Nyx installed to " << install_dir << '\n';
  if (ask_yes_no("Launch Nyx now?")) {
    nyx_setup::launch_app(install_dir + "/nyx-app-wrapper.sh");
  }
  return 0;
}
