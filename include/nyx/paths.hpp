#pragma once

/** @file paths.hpp
 *  Пути к профилю и данным приложения (OS-specific).
 */

#include <string>

namespace nyx {

/** Каталог данных Nyx, например %APPDATA%/nyx или ~/.config/nyx. */
std::string data_dir();

/** Путь к файлу профиля по умолчанию: data_dir()/profile.json. */
std::string default_profile_path();

/** Путь к локальной книге контактов: data_dir()/contacts.json. */
std::string default_contacts_path();

/** Создаёт data_dir(), если его ещё нет. */
bool ensure_data_dir();

/** Каталог загрузок: data_dir()/downloads. */
std::string default_downloads_dir();

/** Каталог логов: data_dir()/logs. */
std::string default_logs_dir();

/** Путь к основному log-файлу: default_logs_dir()/nyx.log. */
std::string default_log_file_path();

}  // namespace nyx
