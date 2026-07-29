#pragma once

#include <string>

namespace nyx {

std::string data_root();

void set_base_data_root(const std::string& root);

std::string data_dir();

void set_account_data_dir(const std::string& account_dir);

void clear_account_data_dir();

std::string accounts_root();

std::string registry_path();

std::string legacy_profile_path();

std::string default_profile_path();

constexpr const char* kEncryptedProfileFilename = "profile.nyx";

std::string default_contacts_path();

bool ensure_data_dir();

std::string default_downloads_dir();

std::string default_logs_dir();

std::string default_log_file_path();

}
