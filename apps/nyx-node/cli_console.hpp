#pragma once

/** @file cli_console.hpp
 *  Потокобезопасный вывод CLI-чата: сообщения, события, подсказка ввода.
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nyx_node {

/** Строка для print_history. */
struct HistoryLine {
  uint64_t timestamp_ms = 0;
  std::string author;
  std::string text;
  bool outgoing = false;
};

/** Форматированный вывод в терминал без конфликтов сетевого потока и stdin. */
class CliConsole {
 public:
  explicit CliConsole(std::string self_nickname);

  /** Шапка сессии после подключения. */
  void print_header(const std::string& peer_nickname, const std::string& peer_id_short);

  /** Системное событие (подключение, отключение, ошибка). */
  void print_event(const std::string& text);

  /** Строка чата. outgoing=true — ваше сообщение. */
  void print_message(uint64_t timestamp_ms, const std::string& author,
                     const std::string& text, bool outgoing);

  /** Печать истории из хранилища. */
  void print_history(const std::vector<HistoryLine>& lines);

  /** Подсказка ввода внизу экрана. */
  void print_prompt();

  /** Справка по командам. */
  void print_help() const;

  /** Статус соединения и peer. */
  void print_status(const std::string& peer_nickname, const std::string& peer_endpoint,
                    bool connected) const;

  const std::string& self_nickname() const { return self_nickname_; }

 private:
  static std::string format_time(uint64_t timestamp_ms);
  void write_line(const std::string& line);

  mutable std::mutex mutex_;
  std::string self_nickname_;
};

}  // namespace nyx_node
