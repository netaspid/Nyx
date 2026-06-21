#include "nyx/group.hpp"

#include "nyx/paths.hpp"
#include "nyx/messaging.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
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

bool GroupStore::load() {
  groups_.clear();
  std::ifstream file(store_path(), std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();

  std::size_t pos = 0;
  while ((pos = json.find("\"group_id\":\"", pos)) != std::string::npos) {
    GroupRecord group;
    pos += 12;
    const auto end = json.find('"', pos);
    if (end == std::string::npos) break;
    if (!group_id_from_hex(json.substr(pos, end - pos), group.id)) {
      pos = end;
      continue;
    }
    pos = end;
    const auto obj_start = json.rfind('{', pos);
    const auto obj_end = json.find('}', pos);
    if (obj_start == std::string::npos || obj_end == std::string::npos) break;
    const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

    const auto name_pos = obj.find("\"name\":\"");
    if (name_pos != std::string::npos) {
      const auto ns = name_pos + 8;
      const auto ne = obj.find('"', ns);
      if (ne != std::string::npos) group.name = obj.substr(ns, ne - ns);
    }
    const auto inv_pos = obj.find("\"invite\":\"");
    if (inv_pos != std::string::npos) {
      const auto is = inv_pos + 10;
      const auto ie = obj.find('"', is);
      if (ie != std::string::npos) {
        invite_from_hex(obj.substr(is, ie - is), group.invite_token);
      }
    }
    const auto owner_pos = obj.find("\"owner\":\"");
    if (owner_pos != std::string::npos) {
      const auto os = owner_pos + 9;
      const auto oe = obj.find('"', os);
      if (oe != std::string::npos) {
        ByteBuffer ob;
        if (from_hex(obj.substr(os, oe - os), ob) && ob.size() == 32) {
          std::memcpy(group.owner_id.data(), ob.data(), 32);
        }
      }
    }
    groups_.push_back(std::move(group));
    pos = obj_end;
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
