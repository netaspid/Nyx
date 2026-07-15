#pragma once

/** @file session_types.hpp
 *  Типы сетевых сессий AppCore (multi-session).
 */

#include <cstdint>
#include <string>

namespace nyx_app {

enum class SessionKind : uint8_t {
  Idle = 0,
  DmInbox = 1,
  Direct = 2,
  GroupHub = 3,
  GroupMember = 4,
};

enum class SessionState : uint8_t {
  Idle = 0,
  Connecting = 1,
  Live = 2,
  Offline = 3,
  Disconnected = 4,
};

/** Снимок сессии для UI / status bar. */
struct SessionInfo {
  std::string id;
  SessionKind kind = SessionKind::Idle;
  SessionState state = SessionState::Idle;
  std::string title;
  std::string ref_id_hex;
};

inline const char* session_state_name(SessionState s) {
  switch (s) {
    case SessionState::Connecting:
      return "connecting";
    case SessionState::Live:
      return "live";
    case SessionState::Offline:
      return "offline";
    case SessionState::Disconnected:
      return "disconnected";
    default:
      return "idle";
  }
}

inline std::string make_dm_session_id(const std::string& peer_hex) {
  return "dm:" + peer_hex;
}

inline std::string make_group_session_id(const std::string& group_hex) {
  return "group:" + group_hex;
}

inline constexpr const char* kDmInboxSessionId = "inbox";

}  // namespace nyx_app
