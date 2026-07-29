#pragma once

/** @file cli_console.hpp
 *  Thread-safe CLI chat output: messages, events, input prompt.
 */

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nyx_node {

/** Line item for print_history. */
struct HistoryLine {
  uint64_t timestamp_ms = 0;
  std::string author;
  std::string text;
  bool outgoing = false;
};

/** Formatted terminal output without clashes between the network thread and stdin. */
class CliConsole {
public:
  explicit CliConsole(std::string self_nickname);

  /** Session header after connect. */
  void print_header(const std::string& peer_nickname, const std::string& peer_id_short);

  /** System event (connect, disconnect, error). */
  void print_event(const std::string& text);

  /** Chat line. outgoing=true marks own messages. */
  void print_message(uint64_t timestamp_ms,
                     const std::string& author,
                     const std::string& text,
                     bool outgoing);

  /** Prints stored history. */
  void print_history(const std::vector<HistoryLine>& lines);

  /** Input prompt at the bottom of the screen. */
  void print_prompt();

  /** Command help. */
  void print_help() const;

  /** Connection and peer status. */
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
