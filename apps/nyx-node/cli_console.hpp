#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nyx_node {

struct HistoryLine {
  uint64_t timestamp_ms = 0;
  std::string author;
  std::string text;
  bool outgoing = false;
};

class CliConsole {
public:
  explicit CliConsole(std::string self_nickname);

  void print_header(const std::string& peer_nickname, const std::string& peer_id_short);

  void print_event(const std::string& text);

  void print_message(uint64_t timestamp_ms,
                     const std::string& author,
                     const std::string& text,
                     bool outgoing);

  void print_history(const std::vector<HistoryLine>& lines);

  void print_prompt();

  void print_help() const;

  void print_status(const std::string& peer_nickname,
                    const std::string& peer_endpoint,
                    bool connected) const;

  const std::string& self_nickname() const { return self_nickname_; }

private:
  static std::string format_time(uint64_t timestamp_ms);
  void write_line(const std::string& line);

  mutable std::mutex mutex_;
  std::string self_nickname_;
};

} // namespace nyx_node
