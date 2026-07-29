#pragma once

/** @file session_intent.hpp
 *  Desired sessions after startup: auto-reconnect / user disconnect.
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

/** Desired session kind. */
enum class SessionIntentKind : uint8_t {
  GroupHub = 1,
  GroupJoin = 2,
  Direct = 3,
};

/** Record: whether to bring the session up automatically. */
struct SessionIntent {
  SessionIntentKind kind = SessionIntentKind::Direct;
  std::string key;        /**< dm:<hex> | group:<hex> */
  std::string ref_id_hex; /**< peer or group id */
  std::string invite_hex; /**< join invite / outbound DM token */
  bool enabled = true;    /**< false after an explicit disconnect */
  uint64_t updated_ms = 0;
};

/** Persists session_intents.json in the account data_dir. */
class SessionIntentStore {
public:
  explicit SessionIntentStore(std::string path = {});

  /** Reads intents from disk. @return true even when the file does not exist yet. */
  bool load();
  /** Writes intents to disk. @return false on write error. */
  bool save() const;

  const std::vector<SessionIntent>& all() const { return intents_; }

  /** Upsert by key; refreshes updated_ms. */
  void upsert(SessionIntent intent);
  /** Marks intent.enabled = false (or creates a disabled record). */
  void disable(const std::string& key);
  /** Enables an existing intent or creates an enabled one. */
  void enable(SessionIntent intent);
  /** @return true only when key exists and is enabled; unknown key = false. */
  bool is_enabled(const std::string& key) const;
  const SessionIntent* find(const std::string& key) const;

private:
  std::string path_;
  std::vector<SessionIntent> intents_;
};

/** Path data_dir()/session_intents.json. */
std::string default_session_intents_path();

/** Loads or creates the stable DM-inbox InviteToken. */
bool load_or_create_dm_inbox_token(InviteToken& out);
/** Hex of the stable inbox token (empty string on error). */
std::string dm_inbox_token_hex();

} // namespace nyx
