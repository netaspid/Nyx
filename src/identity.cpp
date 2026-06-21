#include "nyx/identity.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <crypto/ed25519/ed25519.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace nyx {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::optional<std::string> json_get_string(const std::string& json,
                                           const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"') break;
    if (c == '\\' && i < json.size()) {
      const char esc = json[i++];
      if (esc == 'n')
        out.push_back('\n');
      else if (esc == 'r')
        out.push_back('\r');
      else if (esc == 't')
        out.push_back('\t');
      else
        out.push_back(esc);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool parse_key_hex(const std::string& hex, std::array<uint8_t, 32>& out) {
  ByteBuffer bytes;
  if (!from_hex(hex, bytes) || bytes.size() != 32) return false;
  std::memcpy(out.data(), bytes.data(), 32);
  return true;
}

std::string default_nickname() {
#ifdef _WIN32
  if (const char* user = std::getenv("USERNAME")) return std::string(user);
#else
  if (const char* user = std::getenv("USER")) return std::string(user);
#endif
  return "user";
}

}  // namespace

std::string short_user_id(const UserId& id) {
  return to_hex(id.data(), 4);
}

Profile generate_profile(const std::string& nickname) {
  Profile profile;
  profile.nickname = nickname.empty() ? default_nickname() : nickname;
  random_bytes(profile.secret_key.data(), profile.secret_key.size());
  ed25519_publickey(profile.secret_key.data(), profile.public_key.data());
  return profile;
}

bool save_profile(const std::string& path, const Profile& profile) {
  ensure_data_dir();
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"v\":1,\"nickname\":\"" << json_escape(profile.nickname)
       << "\",\"sk\":\"" << to_hex(profile.secret_key.data(), profile.secret_key.size())
       << "\",\"pk\":\"" << to_hex(profile.public_key.data(), profile.public_key.size())
       << "\"}\n";
  return static_cast<bool>(file);
}

bool load_profile(const std::string& path, Profile& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();

  auto nickname = json_get_string(json, "nickname");
  auto sk = json_get_string(json, "sk");
  auto pk = json_get_string(json, "pk");
  if (!nickname || !sk || !pk) return false;

  Profile profile;
  profile.nickname = *nickname;
  if (!parse_key_hex(*sk, profile.secret_key)) return false;
  if (!parse_key_hex(*pk, profile.public_key)) return false;
  out = std::move(profile);
  return true;
}

Profile load_or_create_profile(const std::string& path, const std::string& nickname) {
  Profile profile;
  if (load_profile(path, profile)) {
    if (!nickname.empty()) {
      profile.nickname = nickname;
      save_profile(path, profile);
    }
    return profile;
  }
  profile = generate_profile(nickname.empty() ? default_nickname() : nickname);
  save_profile(path, profile);
  return profile;
}

ContactBook::ContactBook(std::string path) : path_(std::move(path)) {}

bool ContactBook::load() {
  contacts_.clear();
  std::ifstream file(path_, std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  std::size_t pos = 0;
  while ((pos = json.find("\"id\":\"", pos)) != std::string::npos) {
    pos += 6;
    const auto id_end = json.find('"', pos);
    if (id_end == std::string::npos) break;
    const std::string id_hex = json.substr(pos, id_end - pos);
    pos = id_end;

    Contact contact;
    if (!parse_key_hex(id_hex, contact.user_id)) continue;

    const auto nick_key = json.find("\"nickname\":\"", pos);
    if (nick_key == std::string::npos) break;
    const auto nick_start = nick_key + 12;
    const auto nick_end = json.find('"', nick_start);
    if (nick_end == std::string::npos) break;
    contact.nickname = json.substr(nick_start, nick_end - nick_start);
    const auto seen_key = json.find("\"last_seen\":", pos);
    if (seen_key != std::string::npos && seen_key < nick_end + 64) {
      try {
        contact.last_seen_ms =
            std::stoull(json.substr(seen_key + 12));
      } catch (const std::exception&) {
        contact.last_seen_ms = 0;
      }
    }
    contacts_.push_back(std::move(contact));
    pos = nick_end;
  }
  return true;
}

bool ContactBook::save() const {
  ensure_data_dir();
  std::ofstream file(path_, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"v\":1,\"contacts\":[";
  for (std::size_t i = 0; i < contacts_.size(); ++i) {
    if (i > 0) file << ',';
    const auto& c = contacts_[i];
    file << "{\"id\":\"" << to_hex(c.user_id.data(), c.user_id.size())
         << "\",\"nickname\":\"" << json_escape(c.nickname)
         << "\",\"trust\":" << static_cast<int>(c.trust_level)
         << ",\"last_seen\":" << c.last_seen_ms << "}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

void ContactBook::upsert(Contact contact) {
  for (auto& existing : contacts_) {
    if (existing.user_id == contact.user_id) {
      existing.nickname = contact.nickname;
      existing.trust_level = contact.trust_level;
      if (contact.last_seen_ms != 0) existing.last_seen_ms = contact.last_seen_ms;
      return;
    }
  }
  contacts_.push_back(std::move(contact));
}

}  // namespace nyx
