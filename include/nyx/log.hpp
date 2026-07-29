#pragma once

/** @file log.hpp
 *  File logging: levels, size-based rotation.
 */

#include <string>

namespace nyx {

enum class LogLevel {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

/** Initializes logs/ and opens nyx.log. Safe to call repeatedly. */
void log_init();

/** Path of the current log file after log_init(). */
std::string default_log_path();

void log_write(LogLevel level, const std::string& message);

inline void log_info(const std::string& msg) {
  log_write(LogLevel::Info, msg);
}

} // namespace nyx
