#pragma once

#include <string>

namespace nyx {

enum class LogLevel {
  Debug = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

void log_init();

std::string default_log_path();

void log_write(LogLevel level, const std::string& message);

inline void log_info(const std::string& msg) {
  log_write(LogLevel::Info, msg);
}

}
