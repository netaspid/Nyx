#include "nyx/group.hpp"

#include "nyx/paths.hpp"
#include "nyx/messaging.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
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

std::vector<std::string> split_objects(const std::string& arr) {
  std::vector<std::string> out;
  std::size_t depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const char c = arr[i];
    if (c == '{') {
      if (depth++ == 0) start = i;
    } else if (c == '}') {
      if (--depth == 0 && start != std::string::npos) {
        out.push_back(arr.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return out;
}

std::optional<std::pair<std::size_t, std::size_t>> json_array_bounds(const std::string& json,
                                                                     std::size_t from) {
  const auto start = json.find('[', from);
  if (start == std::string::npos) return std::nullopt;
  int depth = 0;
  for (std::size_t i = start; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '[')
      ++depth;
    else if (c == ']') {
      --depth;
      if (depth == 0) return std::make_pair(start, i);
    }
  }
  return std::nullopt;
}

void parse_object_array(const std::string& obj, const char* key,
                        const std::function<void(const std::string&)>& on_object) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto key_pos = obj.find(needle);
  if (key_pos == std::string::npos) return;
  const auto bounds = json_array_bounds(obj, key_pos + needle.size());
  if (!bounds) return;
  const auto [as, ae] = *bounds;
  for (const auto& item : split_objects(obj.substr(as, ae - as + 1))) on_object(item);
}

GroupMemberRecord parse_member_object(const std::string& obj) {
  GroupMemberRecord member;
  if (auto uid = json_get_string(obj, "user_id")) {
    ByteBuffer buf;
    if (from_hex(*uid, buf) && buf.size() == member.user_id.size()) {
      std::memcpy(member.user_id.data(), buf.data(), buf.size());
    }
  }
  if (auto nick = json_get_string(obj, "nickname")) member.nickname = *nick;
  if (auto role = json_get_string(obj, "role")) {
    if (*role == "owner")
      member.role = GroupRole::Owner;
    else
      member.role = GroupRole::Member;
  }
  return member;
}

std::string role_to_string(GroupRole role) {
  return role == GroupRole::Owner ? "owner" : "member";
}

bool user_id_is_zero(const UserId& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
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

  parse_object_array(obj, "members", [&](const std::string& member_obj) {
    auto member = parse_member_object(member_obj);
    if (user_id_is_zero(member.user_id)) return;
    group.members.push_back(std::move(member));
  });

  if (auto d = json_get_string(obj, "description")) group.description = *d;
  if (auto dir = json_get_string(obj, "direction")) group.direction = *dir;
  if (auto tags = json_get_string(obj, "tags")) group.tags = *tags;
  if (auto vis = json_get_string(obj, "visibility")) {
    group.visibility =
        (*vis == "public") ? GroupVisibility::PublicListed : GroupVisibility::Circle;
  }

  GroupStore::ensure_roster(group);
  return group;
}

}  // namespace

GroupStore::GroupStore() { load(); }

void GroupStore::merge_member_roster(std::vector<GroupMemberRecord>& target,
                                     const std::vector<GroupMemberRecord>& live) {
  for (const auto& m : live) {
    if (user_id_is_zero(m.user_id)) continue;
    bool found = false;
    for (auto& existing : target) {
      if (existing.user_id != m.user_id) continue;
      if (!m.nickname.empty()) existing.nickname = m.nickname;
      if (m.role == GroupRole::Owner) existing.role = GroupRole::Owner;
      found = true;
      break;
    }
    if (!found) target.push_back(m);
  }
}

void GroupStore::ensure_roster(GroupRecord& group, const std::string& owner_nickname_fallback) {
  if (user_id_is_zero(group.owner_id)) {
    for (const auto& m : group.members) {
      if (m.role == GroupRole::Owner) {
        group.owner_id = m.user_id;
        break;
      }
    }
  }
  if (user_id_is_zero(group.owner_id)) return;

  bool has_owner = false;
  for (auto& m : group.members) {
    if (m.user_id != group.owner_id) continue;
    has_owner = true;
    m.role = GroupRole::Owner;
    if (m.nickname.empty() && !owner_nickname_fallback.empty()) m.nickname = owner_nickname_fallback;
    break;
  }
  if (!has_owner) {
    GroupMemberRecord owner;
    owner.user_id = group.owner_id;
    owner.role = GroupRole::Owner;
    owner.nickname = owner_nickname_fallback.empty() ? "Создатель" : owner_nickname_fallback;
    group.members.insert(group.members.begin(), std::move(owner));
  }
}

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
  std::memcpy(out.data(), bytes.data(), bytes.size());
  return true;
}

std::string GroupStore::invite_hex(const InviteToken& token) {
  return to_hex(token.data(), token.size());
}

bool GroupStore::invite_from_hex(const std::string& hex, InviteToken& out) {
  ByteBuffer bytes;
  if (!from_hex(hex, bytes) || bytes.size() != out.size()) return false;
  std::memcpy(out.data(), bytes.data(), bytes.size());
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

bool GroupStore::update_meta(const GroupId& id, const std::string& description,
                             const std::string& direction, const std::string& tags,
                             GroupVisibility visibility) {
  for (auto& g : groups_) {
    if (g.id != id) continue;
    g.description = description;
    g.direction = direction;
    g.tags = tags;
    g.visibility = visibility;
    return save();
  }
  return false;
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
  GroupRecord copy = group;
  ensure_roster(copy);
  for (auto& g : groups_) {
    if (g.id == copy.id) {
      g = copy;
      return save();
    }
  }
  groups_.push_back(copy);
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
         << "\",\"owner\":\"" << to_hex(g.owner_id.data(), g.owner_id.size())
         << "\",\"description\":\"" << json_escape(g.description) << "\",\"direction\":\""
         << json_escape(g.direction) << "\",\"tags\":\"" << json_escape(g.tags)
         << "\",\"visibility\":\""
         << (g.visibility == GroupVisibility::PublicListed ? "public" : "circle")
         << "\",\"members\":[";
    for (std::size_t mi = 0; mi < g.members.size(); ++mi) {
      if (mi > 0) file << ',';
      const auto& m = g.members[mi];
      file << "{\"user_id\":\"" << to_hex(m.user_id.data(), m.user_id.size()) << "\",\"nickname\":\""
           << json_escape(m.nickname) << "\",\"role\":\"" << role_to_string(m.role) << "\"}";
    }
    file << "]}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

}  // namespace nyx
