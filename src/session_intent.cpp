#include "nyx/session_intent.hpp"

#include "nyx/messaging.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace nyx {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else
      out += c;
  }
  return out;
}

std::optional<std::string> json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"') break;
    if (c == '\\' && i < json.size())
      out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

bool json_get_bool(const std::string& json, const char* key, bool def) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return def;
  const auto rest = json.substr(pos + needle.size(), 5);
  if (rest.rfind("true", 0) == 0) return true;
  if (rest.rfind("false", 0) == 0) return false;
  return def;
}

uint64_t json_get_u64(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return 0;
  try {
    return std::stoull(json.substr(pos + needle.size()));
  } catch (...) {
    return 0;
  }
}

std::string kind_to_str(SessionIntentKind k) {
  switch (k) {
    case SessionIntentKind::GroupHub:
      return "hub";
    case SessionIntentKind::GroupJoin:
      return "join";
    default:
      return "dm";
  }
}

SessionIntentKind kind_from_str(const std::string& s) {
  if (s == "hub") return SessionIntentKind::GroupHub;
  if (s == "join") return SessionIntentKind::GroupJoin;
  return SessionIntentKind::Direct;
}

std::string dm_inbox_path() { return data_dir() + "/dm_inbox.token"; }

}  // namespace

std::string default_session_intents_path() {
  return data_dir() + "/session_intents.json";
}

SessionIntentStore::SessionIntentStore(std::string path)
    : path_(path.empty() ? default_session_intents_path() : std::move(path)) {}

bool SessionIntentStore::load() {
  intents_.clear();
  std::ifstream in(path_, std::ios::binary);
  if (!in) return true;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();

  std::size_t pos = 0;
  while ((pos = json.find("\"key\":\"", pos)) != std::string::npos) {
    const auto obj_start = json.rfind('{', pos);
    const auto obj_end = json.find('}', pos);
    if (obj_start == std::string::npos || obj_end == std::string::npos) break;
    const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
    SessionIntent intent;
    if (auto k = json_get_string(obj, "key")) intent.key = *k;
    if (auto r = json_get_string(obj, "ref")) intent.ref_id_hex = *r;
    if (auto inv = json_get_string(obj, "invite")) intent.invite_hex = *inv;
    if (auto kind = json_get_string(obj, "kind")) intent.kind = kind_from_str(*kind);
    intent.enabled = json_get_bool(obj, "enabled", true);
    intent.updated_ms = json_get_u64(obj, "updated");
    if (!intent.key.empty()) intents_.push_back(std::move(intent));
    pos = obj_end;
  }
  return true;
}

bool SessionIntentStore::save() const {
  ensure_data_dir();
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "{\"v\":1,\"intents\":[";
  for (std::size_t i = 0; i < intents_.size(); ++i) {
    if (i > 0) out << ',';
    const auto& it = intents_[i];
    out << "{\"key\":\"" << json_escape(it.key) << "\",\"kind\":\"" << kind_to_str(it.kind)
        << "\",\"ref\":\"" << json_escape(it.ref_id_hex) << "\",\"invite\":\""
        << json_escape(it.invite_hex) << "\",\"enabled\":" << (it.enabled ? "true" : "false")
        << ",\"updated\":" << it.updated_ms << "}";
  }
  out << "]}\n";
  return static_cast<bool>(out);
}

void SessionIntentStore::upsert(SessionIntent intent) {
  intent.updated_ms = now_ms();
  for (auto& existing : intents_) {
    if (existing.key == intent.key) {
      existing = std::move(intent);
      return;
    }
  }
  intents_.push_back(std::move(intent));
}

void SessionIntentStore::disable(const std::string& key) {
  for (auto& existing : intents_) {
    if (existing.key == key) {
      existing.enabled = false;
      existing.updated_ms = now_ms();
      return;
    }
  }
  SessionIntent intent;
  intent.key = key;
  intent.enabled = false;
  intent.updated_ms = now_ms();
  intents_.push_back(std::move(intent));
}

void SessionIntentStore::enable(SessionIntent intent) {
  intent.enabled = true;
  upsert(std::move(intent));
}

bool SessionIntentStore::is_enabled(const std::string& key) const {
  for (const auto& it : intents_) {
    if (it.key == key) return it.enabled;
  }
  return true;
}

const SessionIntent* SessionIntentStore::find(const std::string& key) const {
  for (const auto& it : intents_) {
    if (it.key == key) return &it;
  }
  return nullptr;
}

bool load_or_create_dm_inbox_token(InviteToken& out) {
  ensure_data_dir();
  const std::string path = dm_inbox_path();
  {
    std::ifstream in(path);
    std::string hex;
    if (in && (in >> hex) && hex.size() == 64) {
      ByteBuffer bytes;
      if (from_hex(hex, bytes) && bytes.size() == out.size()) {
        std::memcpy(out.data(), bytes.data(), out.size());
        return true;
      }
    }
  }
  random_bytes(out.data(), out.size());
  std::ofstream file(path, std::ios::trunc);
  if (!file) return false;
  file << to_hex(out.data(), out.size()) << '\n';
  return static_cast<bool>(file);
}

std::string dm_inbox_token_hex() {
  InviteToken token{};
  if (!load_or_create_dm_inbox_token(token)) return {};
  return to_hex(token.data(), token.size());
}

}  // namespace nyx
