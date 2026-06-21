#pragma once

/** @file account_store.hpp
 *  Локальные аккаунты P2P: реестр, разблокировка, блокировка сессии.
 *  Нет серверной регистрации — identity = Ed25519 ключи, защищённые паролем.
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
};

/** Список аккаунтов из registry.json (без секретов). */
std::vector<AccountMeta> list_accounts();

/** Каталог данных аккаунта: data_root()/accounts/<id>/. */
std::string account_data_dir(const std::string& account_id);

/** Создаёт аккаунт: ключи, шифрование, запись в реестр. */
bool create_account(const std::string& nickname, const std::string& password,
                    AccountMeta* created = nullptr, std::string* err = nullptr);

/** Разблокирует аккаунт: проверка пароля, scope data_dir, lock-файл. */
bool unlock_account(const std::string& account_id, const std::string& password,
                    Profile* profile_out = nullptr, std::string* err = nullptr);

/** Завершает сессию и снимает lock. */
void lock_session();

/** Активный аккаунт в этой сессии (пусто если не разблокирован). */
std::string active_account_id();

/** Активный профиль текущей сессии. */
bool active_profile(Profile& out);

/** Обновляет никнейм активной сессии и сохраняет на диск. */
bool update_session_nickname(const std::string& nickname, std::string* err = nullptr);

/** Импорт legacy profile.json в новый зашифрованный аккаунт. */
bool import_legacy_profile(const std::string& password, AccountMeta* created = nullptr,
                           std::string* err = nullptr);

/** Есть ли неимпортированный profile.json в корне data. */
bool legacy_profile_pending();

}  // namespace nyx
