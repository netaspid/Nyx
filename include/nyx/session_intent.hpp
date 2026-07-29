#pragma once

#include "nyx/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

enum class SessionIntentKind : uint8_t {
  GroupHub = 1,
  GroupJoin = 2,
  Direct = 3,
};

struct SessionIntent {
  SessionIntentKind kind = SessionIntentKind::Direct;
  std::string key;
  std::string ref_id_hex;
  std::string invite_hex;
  bool enabled = true;
  uint64_t updated_ms = 0;
};

class SessionIntentStore {
public:
  explicit SessionIntentStore(std::string path = {});


  bool load();

  bool save() const;

  const std::vector<SessionIntent>& all() const { return intents_; }


  void upsert(SessionIntent intent);

  void disable(const std::string& key);

  void enable(SessionIntent intent);

  bool is_enabled(const std::string& key) const;
  const SessionIntent* find(const std::string& key) const;

private:
  std::string path_;
  std::vector<SessionIntent> intents_;
};

std::string default_session_intents_path();

bool load_or_create_dm_inbox_token(InviteToken& out);

std::string dm_inbox_token_hex();

}
