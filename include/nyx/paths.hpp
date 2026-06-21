#pragma once

/** @file paths.hpp
 *  Пути к профилю и данным приложения (OS-specific).
 */

#include <string>

namespace nyx {

/** Корень данных Nyx (%APPDATA%/nyx), без учёта активного аккаунта. */
std::string data_root();

/** Каталог данных: корень или каталог активного аккаунта. */
std::string data_dir();

/** Переключает data_dir() на каталог аккаунта (после unlock). */
void set_account_data_dir(const std::string& account_dir);

/** Сбрасывает scope аккаунта. */
void clear_account_data_dir();

/** accounts/ под корнем. */
std::string accounts_root();

/** registry.json — список аккаунтов без секретов. */
std::string registry_path();

/** Старый plaintext profile.json для миграции. */
std::string legacy_profile_path();

/** Зашифрованный профиль активного аккаунта: data_dir()/profile.nyx. */
std::string default_profile_path();

/** Имя файла зашифрованного профиля. */
constexpr const char* kEncryptedProfileFilename = "profile.nyx";

/** Путь к локальной книге контактов. */
std::string default_contacts_path();

/** Создаёт data_dir(), если его ещё нет. */
bool ensure_data_dir();

/** Каталог загрузок. */
std::string default_downloads_dir();

/** Каталог логов. */
std::string default_logs_dir();

/** Путь к основному log-файлу. */
std::string default_log_file_path();

}  // namespace nyx
