#pragma once

/** @file paths.hpp
 *  Profile and application data paths (OS-specific).
 */

#include <string>

namespace nyx {

/** Nyx data root (%APPDATA%/nyx), independent of the active account. */
std::string data_root();

/** Override OS default root (e.g. Android AppDataLocation). Empty clears override. */
void set_base_data_root(const std::string& root);

/** Data directory: the root or the active account directory. */
std::string data_dir();

/** Points data_dir() at the account directory (after unlock). */
void set_account_data_dir(const std::string& account_dir);

/** Resets the account scope. */
void clear_account_data_dir();

/** accounts/ under the root. */
std::string accounts_root();

/** registry.json: account list without secrets. */
std::string registry_path();

/** Legacy plaintext profile.json used for migration. */
std::string legacy_profile_path();

/** Encrypted profile of the active account: data_dir()/profile.nyx. */
std::string default_profile_path();

/** Encrypted profile file name. */
constexpr const char* kEncryptedProfileFilename = "profile.nyx";

/** Local contact book path. */
std::string default_contacts_path();

/** Creates data_dir() when missing. */
bool ensure_data_dir();

/** Downloads directory. */
std::string default_downloads_dir();

/** Logs directory. */
std::string default_logs_dir();

/** Main log file path. */
std::string default_log_file_path();

}  // namespace nyx
