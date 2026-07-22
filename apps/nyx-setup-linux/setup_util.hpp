#pragma once

#include "payload.hpp"

#include <string>
#include <vector>

namespace nyx_setup {

bool stop_nyx_for_install(const std::string& install_dir, std::string* err = nullptr);

bool create_desktop_entry(const std::string& install_dir);

bool remove_desktop_entry();

bool register_install_manifest(const std::string& install_dir);

bool launch_app(const std::string& exe_path);

bool verify_installation(const std::string& install_dir, std::string* err = nullptr);

bool repair_installation(const std::vector<std::uint8_t>& blob, const std::string& install_dir,
                         std::string* err = nullptr);

std::string default_install_dir();

std::string data_root();

}  // namespace nyx_setup
