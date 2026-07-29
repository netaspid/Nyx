#pragma once

/** @file account_store.hpp
 *  Local accounts: registry, unlock, recovery, remember-me.
 *  No server-side registration; identity = Ed25519 keys protected by a password.
 */

#include "nyx/identity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

struct AccountMeta {
  std::string id;
  std::string nickname;
  uint64_t created_ms = 0;
  bool locked = false;
  bool has_recovery = false;
  bool remember_active = false;
};

/** Remember-me lifetime (30 days). */
inline constexpr std::int64_t kRememberMeDays = 30;

/** Account list from registry.json (no secrets). */
std::vector<AccountMeta> list_accounts();

/** Account data directory: data_root()/accounts/<id>/. */
std::string account_data_dir(const std::string& account_id);

/** Last selected account (used for UI preselection). */
std::string last_account_id();
void set_last_account_id(const std::string& account_id);

/** Creates an account. @param recovery_phrase_out 12-word BIP39 phrase to show the user. */
bool create_account(const std::string& nickname, const std::string& password,
                    std::string* recovery_phrase_out, AccountMeta* created = nullptr,
                    std::string* err = nullptr);

/** Unlocks the account with a password. remember_me keeps the session for 30 days. */
bool unlock_account(const std::string& account_id, const std::string& password,
                    bool remember_me = false, Profile* profile_out = nullptr,
                    std::string* err = nullptr);

/** Unlocks via the remember token when not expired (OS-bound). */
bool try_unlock_remembered(const std::string& account_id, Profile* profile_out = nullptr,
                           std::string* err = nullptr);

/** Password reset via the recovery phrase; the remember token is invalidated. */
bool reset_password_with_recovery(const std::string& account_id,
                                  const std::string& recovery_phrase,
                                  const std::string& new_password, std::string* err = nullptr);

/** Whether the account has recovery.nyx. */
bool account_has_recovery(const std::string& account_id);

/** Whether a non-expired remember token exists. */
bool account_remember_active(const std::string& account_id);

/** Clears the account remember token (logout / password change). */
void clear_remember_token(const std::string& account_id);

/** Stores a remember token for an already open session. */
bool enable_remember_me(std::string* err = nullptr);

/** Ends the session and releases the process lock. */
void lock_session(bool clear_remember = false);

/** Active account in this session (empty when locked). */
std::string active_account_id();

/** Active profile of the current session. */
bool active_profile(Profile& out);

/** Updates the active session nickname and persists it. */
bool update_session_nickname(const std::string& nickname, std::string* err = nullptr);

/** Imports a legacy profile.json; generates a recovery phrase. */
bool import_legacy_profile(const std::string& password, std::string* recovery_phrase_out,
                           AccountMeta* created = nullptr, std::string* err = nullptr);

/** Whether an unimported profile.json exists in the data root. */
bool legacy_profile_pending();

}  // namespace nyx
