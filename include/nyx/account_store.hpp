#pragma once

/** @file account_store.hpp
 *  Локальные аккаунты: реестр, разблокировка, recovery, remember-me.
 *  Нет серверной регистрации — identity = Ed25519, защищённые паролем.
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

/** Срок «запомнить меня» (30 суток). */
inline constexpr std::int64_t kRememberMeDays = 30;

/** Список аккаунтов из registry.json (без секретов). */
std::vector<AccountMeta> list_accounts();

/** Каталог данных аккаунта: data_root()/accounts/<id>/. */
std::string account_data_dir(const std::string& account_id);

/** Последний выбранный аккаунт (для преселекта в UI). */
std::string last_account_id();
void set_last_account_id(const std::string& account_id);

/** Создаёт аккаунт. @param recovery_phrase_out 12-словная BIP39-фраза (показать пользователю). */
bool create_account(const std::string& nickname, const std::string& password,
                    std::string* recovery_phrase_out, AccountMeta* created = nullptr,
                    std::string* err = nullptr);

/** Разблокирует аккаунт паролем. remember_me — сохранить сессию на 30 дней. */
bool unlock_account(const std::string& account_id, const std::string& password,
                    bool remember_me = false, Profile* profile_out = nullptr,
                    std::string* err = nullptr);

/** Разблокирует по remember-токену, если не истёк (OS-bound). */
bool try_unlock_remembered(const std::string& account_id, Profile* profile_out = nullptr,
                           std::string* err = nullptr);

/** Сброс пароля по recovery-фразе; remember-токен сбрасывается. */
bool reset_password_with_recovery(const std::string& account_id,
                                  const std::string& recovery_phrase,
                                  const std::string& new_password, std::string* err = nullptr);

/** Есть ли recovery.nyx у аккаунта. */
bool account_has_recovery(const std::string& account_id);

/** Активен ли непросроченный remember-токен. */
bool account_remember_active(const std::string& account_id);

/** Сбрасывает remember-токен аккаунта (выход / смена пароля). */
void clear_remember_token(const std::string& account_id);

/** Сохраняет remember-токен для уже открытой сессии. */
bool enable_remember_me(std::string* err = nullptr);

/** Завершает сессию и снимает process-lock. */
void lock_session(bool clear_remember = false);

/** Активный аккаунт в этой сессии (пусто если не разблокирован). */
std::string active_account_id();

/** Активный профиль текущей сессии. */
bool active_profile(Profile& out);

/** Обновляет никнейм активной сессии и сохраняет на диск. */
bool update_session_nickname(const std::string& nickname, std::string* err = nullptr);

/** Импорт legacy profile.json; генерирует recovery-фразу. */
bool import_legacy_profile(const std::string& password, std::string* recovery_phrase_out,
                           AccountMeta* created = nullptr, std::string* err = nullptr);

/** Есть ли неимпортированный profile.json в корне data. */
bool legacy_profile_pending();

}  // namespace nyx
