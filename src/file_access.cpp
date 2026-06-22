#include "nyx/file_access.hpp"
#include "nyx/group.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cctype>
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

std::optional<uint32_t> json_get_uint(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  std::size_t j = i;
  while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j]))) ++j;
  if (j == i) return std::nullopt;
  return static_cast<uint32_t>(std::stoul(json.substr(i, j - i)));
}

std::optional<bool> json_get_bool(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  const auto sub = json.substr(pos + needle.size(), 8);
  if (sub.rfind("true", 0) == 0) return true;
  if (sub.rfind("false", 0) == 0) return false;
  return std::nullopt;
}

bool user_id_from_hex(const std::string& hex, UserId& out) {
  ByteBuffer buf;
  if (!from_hex(hex, buf) || buf.size() != out.size()) return false;
  std::memcpy(out.data(), buf.data(), buf.size());
  return true;
}

std::string user_id_hex(const UserId& id) {
  return to_hex(id.data(), id.size());
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

FileRole parse_role(const std::string& obj) {
  FileRole role;
  if (auto id = json_get_string(obj, "id")) role.id = *id;
  if (auto name = json_get_string(obj, "name")) role.name = *name;
  if (auto perms = json_get_uint(obj, "permissions")) role.permissions = *perms;
  if (auto builtin = json_get_bool(obj, "builtin")) role.builtin = *builtin;
  return role;
}

FileMemberAssignment parse_assignment(const std::string& obj) {
  FileMemberAssignment a;
  if (auto uid = json_get_string(obj, "user_id")) user_id_from_hex(*uid, a.user_id);
  if (auto rid = json_get_string(obj, "role_id")) a.role_id = *rid;
  return a;
}

FileRootGrant parse_root_grant(const std::string& obj) {
  FileRootGrant g;
  if (auto root = json_get_string(obj, "root")) g.root_path = *root;
  if (auto uid = json_get_string(obj, "user_id")) user_id_from_hex(*uid, g.user_id);
  if (auto rid = json_get_string(obj, "role_id")) g.role_id = *rid;
  return g;
}

}  // namespace

FileAccessStore::FileAccessStore() { load(); }

std::string FileAccessStore::store_path() { return data_dir() + "/file_access.json"; }

std::string FileAccessStore::role_id_owner() { return "owner"; }
std::string FileAccessStore::role_id_member() { return "member"; }
std::string FileAccessStore::role_id_viewer() { return "viewer"; }

GroupFileAccess FileAccessStore::default_policy(const GroupId& group_id,
                                                const GroupRecord& group) {
  GroupFileAccess policy;
  policy.group_id = group_id;

  FileRole owner;
  owner.id = role_id_owner();
  owner.name = "Владелец";
  owner.permissions = kFilePermissionAll;
  owner.builtin = true;

  FileRole member;
  member.id = role_id_member();
  member.name = "Участник";
  member.permissions = static_cast<uint32_t>(FilePermission::List) |
                       static_cast<uint32_t>(FilePermission::Download) |
                       static_cast<uint32_t>(FilePermission::Upload) |
                       static_cast<uint32_t>(FilePermission::OpenRemote) |
                       static_cast<uint32_t>(FilePermission::ManageShares);
  member.builtin = true;

  FileRole viewer;
  viewer.id = role_id_viewer();
  viewer.name = "Только чтение";
  viewer.permissions = static_cast<uint32_t>(FilePermission::List) |
                       static_cast<uint32_t>(FilePermission::Download);
  viewer.builtin = true;

  policy.roles = {owner, member, viewer};

  for (const auto& m : group.members) {
    FileMemberAssignment a;
    a.user_id = m.user_id;
    if (m.role == GroupRole::Owner)
      a.role_id = role_id_owner();
    else
      a.role_id = role_id_member();
    policy.assignments.push_back(std::move(a));
  }
  return policy;
}

bool FileAccessStore::load() {
  policies_.clear();
  std::ifstream in(store_path());
  if (!in) return true;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  if (json.empty()) return true;

  const auto groups_key = json.find("\"groups\"");
  if (groups_key == std::string::npos) return true;
  const auto arr_start = json.find('[', groups_key);
  const auto arr_end = json.find(']', arr_start);
  if (arr_start == std::string::npos || arr_end == std::string::npos) return true;

  for (const auto& obj : split_objects(json.substr(arr_start, arr_end - arr_start + 1))) {
    GroupFileAccess policy;
    if (auto gid = json_get_string(obj, "group_id")) {
      GroupStore::group_id_from_hex(*gid, policy.group_id);
    }
    const auto roles_start = obj.find("\"roles\"");
    if (roles_start != std::string::npos) {
      const auto rs = obj.find('[', roles_start);
      const auto re = obj.find(']', rs);
      if (rs != std::string::npos && re != std::string::npos) {
        for (const auto& role_obj : split_objects(obj.substr(rs, re - rs + 1))) {
          auto role = parse_role(role_obj);
          if (!role.id.empty()) policy.roles.push_back(std::move(role));
        }
      }
    }
    const auto assign_start = obj.find("\"assignments\"");
    if (assign_start != std::string::npos) {
      const auto as = obj.find('[', assign_start);
      const auto ae = obj.find(']', as);
      if (as != std::string::npos && ae != std::string::npos) {
        for (const auto& a_obj : split_objects(obj.substr(as, ae - as + 1))) {
          auto a = parse_assignment(a_obj);
          if (!a.role_id.empty()) policy.assignments.push_back(std::move(a));
        }
      }
    }
    const auto grants_start = obj.find("\"root_grants\"");
    if (grants_start != std::string::npos) {
      const auto gs = obj.find('[', grants_start);
      const auto ge = obj.find(']', gs);
      if (gs != std::string::npos && ge != std::string::npos) {
        for (const auto& g_obj : split_objects(obj.substr(gs, ge - gs + 1))) {
          auto g = parse_root_grant(g_obj);
          if (!g.root_path.empty() && !g.role_id.empty()) {
            policy.root_grants.push_back(std::move(g));
          }
        }
      }
    }
    if (!std::all_of(policy.group_id.begin(), policy.group_id.end(),
                     [](uint8_t b) { return b == 0; })) {
      policies_.push_back(std::move(policy));
    }
  }
  return true;
}

bool FileAccessStore::save() const {
  std::ofstream out(store_path(), std::ios::trunc);
  if (!out) return false;
  out << "{\"groups\":[";
  for (std::size_t gi = 0; gi < policies_.size(); ++gi) {
    const auto& p = policies_[gi];
    if (gi) out << ',';
    out << "{\"group_id\":\"" << GroupStore::group_id_hex(p.group_id) << "\",\"roles\":[";
    for (std::size_t ri = 0; ri < p.roles.size(); ++ri) {
      const auto& r = p.roles[ri];
      if (ri) out << ',';
      out << "{\"id\":\"" << json_escape(r.id) << "\",\"name\":\"" << json_escape(r.name)
          << "\",\"permissions\":" << r.permissions << ",\"builtin\":"
          << (r.builtin ? "true" : "false") << "}";
    }
    out << "],\"assignments\":[";
    for (std::size_t ai = 0; ai < p.assignments.size(); ++ai) {
      const auto& a = p.assignments[ai];
      if (ai) out << ',';
      out << "{\"user_id\":\"" << user_id_hex(a.user_id) << "\",\"role_id\":\""
          << json_escape(a.role_id) << "\"}";
    }
    out << "],\"root_grants\":[";
    for (std::size_t gi2 = 0; gi2 < p.root_grants.size(); ++gi2) {
      const auto& g = p.root_grants[gi2];
      if (gi2) out << ',';
      out << "{\"root\":\"" << json_escape(g.root_path) << "\",\"user_id\":\""
          << user_id_hex(g.user_id) << "\",\"role_id\":\"" << json_escape(g.role_id) << "\"}";
    }
    out << "]}";
  }
  out << "]}";
  return static_cast<bool>(out);
}

GroupFileAccess& FileAccessStore::ensure_policy(const GroupId& group_id,
                                                const GroupRecord& group) {
  for (auto& policy : policies_) {
    if (policy.group_id != group_id) continue;
    bool changed = false;
    for (const auto& m : group.members) {
      const bool known = std::any_of(policy.assignments.begin(), policy.assignments.end(),
                                     [&](const FileMemberAssignment& a) {
                                       return a.user_id == m.user_id;
                                     });
      if (!known) {
        FileMemberAssignment a;
        a.user_id = m.user_id;
        a.role_id = m.role == GroupRole::Owner ? role_id_owner() : role_id_member();
        policy.assignments.push_back(std::move(a));
        changed = true;
      }
    }
    if (changed) save();
    return policy;
  }
  policies_.push_back(default_policy(group_id, group));
  save();
  return policies_.back();
}

GroupFileAccess& FileAccessStore::policy_for(const GroupId& group_id) {
  for (auto& p : policies_) {
    if (p.group_id == group_id) return p;
  }
  GroupRecord stub;
  stub.id = group_id;
  GroupFileAccess created = default_policy(group_id, stub);
  policies_.push_back(std::move(created));
  return policies_.back();
}

const GroupFileAccess* FileAccessStore::find_policy(const GroupId& group_id) const {
  for (const auto& p : policies_) {
    if (p.group_id == group_id) return &p;
  }
  return nullptr;
}

uint32_t FileAccessStore::permissions_for(const GroupId& group_id,
                                          const UserId& user_id) const {
  return permissions_for(group_id, user_id, {});
}

uint32_t FileAccessStore::permissions_for(const GroupId& group_id,
                                          const UserId& user_id,
                                          const std::string& root_path) const {
  const GroupFileAccess* policy = find_policy(group_id);
  if (!policy) {
    return static_cast<uint32_t>(FilePermission::List) |
           static_cast<uint32_t>(FilePermission::Download);
  }
  std::string role_id = role_id_viewer();
  if (!root_path.empty()) {
    const std::string root_norm = normalize_utf8_path(root_path);
    for (const auto& g : policy->root_grants) {
      if (normalize_utf8_path(g.root_path) == root_norm && g.user_id == user_id) {
        role_id = g.role_id;
        goto resolve_role;
      }
    }
  }
  for (const auto& a : policy->assignments) {
    if (a.user_id == user_id) {
      role_id = a.role_id;
      break;
    }
  }
resolve_role:
  for (const auto& r : policy->roles) {
    if (r.id == role_id) return r.permissions;
  }
  return static_cast<uint32_t>(FilePermission::List);
}

bool FileAccessStore::set_root_member_role(const GroupId& group_id,
                                           const std::string& root_path,
                                           const UserId& user_id,
                                           const std::string& role_id) {
  if (root_path.empty()) return false;
  auto& policy = policy_for(group_id);
  const std::string root_norm = normalize_utf8_path(root_path);

  if (role_id.empty()) {
    policy.root_grants.erase(
        std::remove_if(policy.root_grants.begin(), policy.root_grants.end(),
                       [&](const FileRootGrant& g) {
                         return normalize_utf8_path(g.root_path) == root_norm &&
                                g.user_id == user_id;
                       }),
        policy.root_grants.end());
    return save();
  }

  for (auto& r : policy.roles) {
    if (r.id != role_id) continue;
    for (auto& g : policy.root_grants) {
      if (normalize_utf8_path(g.root_path) == root_norm && g.user_id == user_id) {
        g.role_id = role_id;
        return save();
      }
    }
    policy.root_grants.push_back({root_norm, user_id, role_id});
    return save();
  }
  return false;
}

bool FileAccessStore::set_member_role(const GroupId& group_id, const UserId& user_id,
                                      const std::string& role_id) {
  auto& policy = policy_for(group_id);
  for (auto& r : policy.roles) {
    if (r.id == role_id) {
      for (auto& a : policy.assignments) {
        if (a.user_id == user_id) {
          a.role_id = role_id;
          return save();
        }
      }
      policy.assignments.push_back({user_id, role_id});
      return save();
    }
  }
  return false;
}

bool FileAccessStore::upsert_role(const GroupId& group_id, const FileRole& role) {
  if (role.id.empty() || role.name.empty()) return false;
  auto& policy = policy_for(group_id);
  for (auto& r : policy.roles) {
    if (r.id == role.id) {
      if (r.builtin && role.id == role_id_owner()) return false;
      r.name = role.name;
      r.permissions = role.permissions;
      return save();
    }
  }
  FileRole copy = role;
  copy.builtin = false;
  policy.roles.push_back(std::move(copy));
  return save();
}

bool FileAccessStore::remove_role(const GroupId& group_id, const std::string& role_id) {
  auto& policy = policy_for(group_id);
  const auto it = std::find_if(policy.roles.begin(), policy.roles.end(),
                               [&](const FileRole& r) { return r.id == role_id; });
  if (it == policy.roles.end() || it->builtin) return false;
  for (auto& a : policy.assignments) {
    if (a.role_id == role_id) a.role_id = role_id_viewer();
  }
  policy.roles.erase(it);
  return save();
}

bool FileAccessStore::has_permission(uint32_t mask, FilePermission perm) {
  return (mask & static_cast<uint32_t>(perm)) != 0;
}

}  // namespace nyx
