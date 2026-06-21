#include "nyx/group.hpp"

#include "nyx/paths.hpp"
#include "nyx/messaging.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace nyx {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
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
    if (c == '\\' && i < json.size()) out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

std::optional<GroupRecord> parse_group_object(const std::string& obj) {
  GroupRecord group;
  std::string gid_hex;
  if (auto g = json_get_string(obj, "group_id")) {
    gid_hex = *g;
  } else if (auto g = json_get_string(obj, "id")) {
    gid_hex = *g;
  } else {
    return std::nullopt;
  }
  if (!GroupStore::group_id_from_hex(gid_hex, group.id)) return std::nullopt;

  if (auto name = json_get_string(obj, "name")) group.name = *name;

  if (auto inv = json_get_string(obj, "invite")) {
    GroupStore::invite_from_hex(*inv, group.invite_token);
  }

  if (auto owner = json_get_string(obj, "owner")) {
    ByteBuffer ob;
    if (from_hex(*owner, ob) && ob.size() == group.owner_id.size()) {
      std::memcpy(group.owner_id.data(), ob.data(), ob.size());
    }
  }

  return group;
}

}  // namespace

GroupStore::GroupStore() { load(); }

std::string GroupStore::store_path() { return data_dir() + "/groups.json"; }

GroupId GroupStore::generate_id() {
  GroupId id{};
  random_bytes(id.data(), id.size());
  return id;
}

InviteToken GroupStore::generate_invite() {
  InviteToken token{};
  random_bytes(token.data(), token.size());
  return token;
}

std::string GroupStore::group_id_hex(const GroupId& id) {
  return to_hex(id.data(), id.size());
}

bool GroupStore::group_id_from_hex(const std::string& hex, GroupId& out) {
  ByteBuffer bytes;
  if (!from_hex(hex, bytes) || bytes.size() != out.size()) return false;
  std::memcpy(out.data(), bytes.data(), out.size());
  return true;
}

std::string GroupStore::invite_hex(const InviteToken& token) {
  return to_hex(token.data(), token.size());
}

bool GroupStore::invite_from_hex(const std::string& hex, InviteToken& out) {
  ByteBuffer bytes;
  if (!from_hex(hex, bytes) || bytes.size() != out.size()) return false;
  std::memcpy(out.data(), bytes.data(), out.size());
  return true;
}

GroupRecord GroupStore::create(const std::string& name, const UserId& owner_id,
                               const std::string& owner_nickname) {
  GroupRecord group;
  group.id = generate_id();
  group.name = name;
  group.owner_id = owner_id;
  group.invite_token = generate_invite();
  group.created_ms = now_ms();
  GroupMemberRecord owner;
  owner.user_id = owner_id;
  owner.nickname = owner_nickname;
  owner.role = GroupRole::Owner;
  group.members.push_back(std::move(owner));
  groups_.push_back(group);
  save();
  return group;
}

std::optional<GroupRecord> GroupStore::find(const GroupId& id) const {
  for (const auto& g : groups_) {
    if (g.id == id) return g;
  }
  return std::nullopt;
}

std::optional<GroupRecord> GroupStore::find_by_invite(const InviteToken& token) const {
  for (const auto& g : groups_) {
    if (g.invite_token == token) return g;
  }
  return std::nullopt;
}

bool GroupStore::upsert(const GroupRecord& group) {
  for (auto& g : groups_) {
    if (g.id == group.id) {
      g = group;
      return save();
    }
  }
  groups_.push_back(group);
  return save();
}

bool GroupStore::remove(const GroupId& id) {
  const auto it =
      std::remove_if(groups_.begin(), groups_.end(), [&](const GroupRecord& g) { return g.id == id; });
  if (it == groups_.end()) return false;
  groups_.erase(it, groups_.end());
  return save();
}

bool GroupStore::remove_member(const GroupId& id, const UserId& user_id) {
  for (auto& g : groups_) {
    if (g.id != id) continue;
    const auto it = std::remove_if(g.members.begin(), g.members.end(),
                                     [&](const GroupMemberRecord& m) { return m.user_id == user_id; });
    if (it == g.members.end()) return false;
    g.members.erase(it, g.members.end());
    return save();
  }
  return false;
}

bool GroupStore::load() {
  groups_.clear();
  std::ifstream file(store_path(), std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();

  const auto arr = json.find("\"groups\":[");
  if (arr == std::string::npos) return true;

  std::size_t pos = arr + 10;
  while (pos < json.size()) {
    pos = json.find('{', pos);
    if (pos == std::string::npos) break;

    int depth = 0;
    const std::size_t start = pos;
    for (; pos < json.size(); ++pos) {
      const char c = json[pos];
      if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          const std::string obj = json.substr(start, pos - start + 1);
          if (auto group = parse_group_object(obj)) {
            groups_.push_back(std::move(*group));
          }
          ++pos;
          break;
        }
      }
    }

    if (pos < json.size() && json[pos] == ']') break;
  }
  return true;
}

bool GroupStore::save() const {
  ensure_data_dir();
  std::ofstream file(store_path(), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"groups\":[";
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    if (i > 0) file << ',';
    const auto& g = groups_[i];
    file << "{\"group_id\":\"" << group_id_hex(g.id) << "\",\"name\":\""
         << json_escape(g.name) << "\",\"invite\":\"" << invite_hex(g.invite_token)
         << "\",\"owner\":\"" << to_hex(g.owner_id.data(), g.owner_id.size()) << "\"}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

}  // namespace nyx
