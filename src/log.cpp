#include "nyx/log.hpp"

#include "nyx/paths.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace nyx {

namespace {

std::mutex log_mutex;
LogLevel min_level = LogLevel::Info;
std::string log_path;
constexpr std::size_t kMaxLogBytes = 5 * 1024 * 1024;

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "?";
}

std::string timestamp_now() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto t = clock::to_time_t(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &t);
#else
  localtime_r(&t, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
      << ms.count();
  return oss.str();
}

void rotate_if_needed() {
  if (log_path.empty()) return;
  std::error_code ec;
  const auto size = std::filesystem::file_size(log_path, ec);
  if (ec || size < kMaxLogBytes) return;
  const std::string backup = log_path + ".1";
  std::filesystem::remove(backup, ec);
  std::filesystem::rename(log_path, backup, ec);
}

}  // namespace

void log_init() {
  std::lock_guard lock(log_mutex);
  if (!log_path.empty()) return;
  ensure_data_dir();
  std::error_code ec;
  std::filesystem::create_directories(default_logs_dir(), ec);
  log_path = default_log_file_path();
  rotate_if_needed();
}

void log_set_level(LogLevel level) { min_level = level; }

LogLevel log_level() { return min_level; }

std::string default_log_path() {
  if (log_path.empty()) log_init();
  return log_path;
}

void log_write(LogLevel level, const std::string& message) {
  if (level < min_level) return;
  if (log_path.empty()) log_init();

  const std::string line = timestamp_now() + " [" + level_name(level) + "] " + message + '\n';

  std::lock_guard lock(log_mutex);
  rotate_if_needed();
  std::ofstream out(log_path, std::ios::app);
  if (out) out << line;
}

}  // namespace nyx
