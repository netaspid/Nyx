#pragma once

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

inline constexpr std::int64_t kRememberMeDays = 30;

std::vector<AccountMeta> list_accounts();

std::string account_data_dir(const std::string& account_id);

std::string last_account_id();
void set_last_account_id(const std::string& account_id);

/** Creates an account. @param recovery_phrase_out 12-word BIP39 phrase to show the user. */
bool create_account(const std::string& nickname,
                    const std::string& password,
                    std::string* recovery_phrase_out,
                    AccountMeta* created = nullptr,
                    std::string* err = nullptr);

bool unlock_account(const std::string& account_id,
                    const std::string& password,
                    bool remember_me = false,
                    Profile* profile_out = nullptr,
                    std::string* err = nullptr);

bool try_unlock_remembered(const std::string& account_id,
                           Profile* profile_out = nullptr,
                           std::string* err = nullptr);

bool reset_password_with_recovery(const std::string& account_id,
                                  const std::string& recovery_phrase,
                                  const std::string& new_password,
                                  std::string* err = nullptr);

bool account_has_recovery(const std::string& account_id);

bool account_remember_active(const std::string& account_id);

void clear_remember_token(const std::string& account_id);

bool enable_remember_me(std::string* err = nullptr);

void lock_session(bool clear_remember = false);

std::string active_account_id();

bool active_profile(Profile& out);

bool update_session_nickname(const std::string& nickname, std::string* err = nullptr);

bool import_legacy_profile(const std::string& password,
                           std::string* recovery_phrase_out,
                           AccountMeta* created = nullptr,
                           std::string* err = nullptr);

bool legacy_profile_pending();

} // namespace nyx
