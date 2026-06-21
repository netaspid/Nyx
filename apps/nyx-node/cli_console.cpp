#include "cli_console.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace nyx_node {

CliConsole::CliConsole(std::string self_nickname)
    : self_nickname_(std::move(self_nickname)) {}

std::string CliConsole::format_time(uint64_t timestamp_ms) {
  const std::time_t sec = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm tm_local{};
#ifdef _WIN32
  localtime_s(&tm_local, &sec);
#else
  localtime_r(&sec, &tm_local);
#endif
  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << tm_local.tm_hour << ':'
      << std::setw(2) << tm_local.tm_min;
  return oss.str();
}

void CliConsole::write_line(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << line << '\n' << std::flush;
}

void CliConsole::print_header(const std::string& peer_nickname,
                              const std::string& peer_id_short) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << "\n"
            << "────────────────────────────────────────\n"
            << "  Nyx · " << self_nickname_ << "\n"
            << "  Собеседник: " << peer_nickname << " (" << peer_id_short << ")\n"
            << "  /help — команды\n"
            << "────────────────────────────────────────\n"
            << std::flush;
}

void CliConsole::print_event(const std::string& text) {
  const auto ts = format_time(
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()));
  write_line('[' + ts + "] * " + text);
}

void CliConsole::print_message(uint64_t timestamp_ms, const std::string& author,
                               const std::string& text, bool outgoing) {
  (void)outgoing;
  write_line('[' + format_time(timestamp_ms) + "] " + author + ": " + text);
}

void CliConsole::print_history(const std::vector<HistoryLine>& lines) {
  if (lines.empty()) {
    print_event("история пуста");
    return;
  }
  for (const auto& line : lines) {
    print_message(line.timestamp_ms, line.author, line.text, line.outgoing);
  }
}

void CliConsole::print_prompt() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << self_nickname_ << "> " << std::flush;
}

void CliConsole::print_help() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout
      << "\nКоманды:\n"
      << "  /help     — эта справка\n"
      << "  /who      — информация о собеседнике\n"
      << "  /status   — состояние соединения\n"
      << "  /history [N] — последние N сообщений (по умолчанию 20)\n"
      << "  /search <текст> — поиск в истории\n"
      << "  /send <текст>  — отправить сообщение\n"
      << "  /index <папка> — добавить папку в индекс файлов\n"
      << "  /files         — локальный список файлов\n"
      << "  /remote        — список файлов у peer\n"
      << "  /get <hash>    — скачать файл по hash\n"
      << "  /sendfile <путь|hash> — отправить файл\n"
      << "  /quit     — выход (отправляет Bye собеседнику)\n"
      << "  /exit     — то же, что /quit\n"
      << "Текст без / — отправка сообщения.\n\n"
      << std::flush;
}

void CliConsole::print_status(const std::string& peer_nickname,
                              const std::string& peer_endpoint,
                              bool connected) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::cout << "\nСтатус:\n"
            << "  Вы: " << self_nickname_ << "\n"
            << "  Собеседник: " << peer_nickname << " @ " << peer_endpoint << "\n"
            << "  Соединение: " << (connected ? "активно" : "разорвано") << "\n\n"
            << std::flush;
}

}  // namespace nyx_node
