#pragma once

/** @file log.hpp
 *  Файловое логирование (фаза 7): уровни, ротация по размеру.
 */

#include <string>

namespace nyx {

enum class LogLevel {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

/** Инициализация: каталог logs/, открытие nyx.log. Безопасно вызывать повторно. */
void log_init();

/** Минимальный уровень для записи в файл (по умолчанию Info). */
void log_set_level(LogLevel level);

LogLevel log_level();

/** Путь к текущему log-файлу после log_init(). */
std::string default_log_path();

void log_write(LogLevel level, const std::string& message);

inline void log_debug(const std::string& msg) { log_write(LogLevel::Debug, msg); }
inline void log_info(const std::string& msg) { log_write(LogLevel::Info, msg); }
inline void log_warn(const std::string& msg) { log_write(LogLevel::Warn, msg); }
inline void log_error(const std::string& msg) { log_write(LogLevel::Error, msg); }

}  // namespace nyx
