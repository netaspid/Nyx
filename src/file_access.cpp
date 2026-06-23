#include "nyx/file_access.hpp"
#include "nyx/group.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
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

/** Индексы открывающей и закрывающей скобки массива, начиная поиск с from. */
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

FilePermissionPreset parse_preset(const std::string& obj) {
  FilePermissionPreset p;
  if (auto id = json_get_string(obj, "id")) p.id = *id;
  if (auto name = json_get_string(obj, "name")) p.name = *name;
  if (auto perms = json_get_uint(obj, "permissions")) p.permissions = *perms;
  return p;
}

FileRootGrant parse_root_grant(const std::string& obj) {
  FileRootGrant g;
  if (auto root = json_get_string(obj, "root")) g.root_path = *root;
  if (auto rel = json_get_string(obj, "rel")) g.relative_path = *rel;
  if (auto uid = json_get_string(obj, "user_id")) user_id_from_hex(*uid, g.user_id);
  if (auto rid = json_get_string(obj, "role_id")) g.role_id = *rid;
  if (auto direct = json_get_uint(obj, "direct")) g.direct_permissions = *direct;
  if (auto direct_only = json_get_bool(obj, "direct_only")) g.direct_only = *direct_only;
  return g;
}

std::string path_to_posix_copy(const std::string& path) {
  std::string out = path;
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
  return out;
}

/** Share-корень, если grant на вложенную папку совпадает с проиндексированным корнем. */
std::string grant_effective_share_root(const FileRootGrant& g) {
  if (g.relative_path.empty()) return normalize_grant_root(g.root_path);
  const auto combined =
      path_from_utf8(g.root_path) / path_from_utf8(g.relative_path);
  return normalize_grant_root(path_to_utf8(combined.lexically_normal()));
}

uint32_t role_permissions(const GroupFileAccess& policy, const std::string& role_id) {
  for (const auto& r : policy.roles) {
    if (r.id == role_id) return r.permissions;
  }
  return static_cast<uint32_t>(FilePermission::List);
}

uint32_t grant_role_permissions(const GroupFileAccess& policy, const FileRootGrant& g) {
  if (g.direct_only) return g.direct_permissions;
  if (!g.role_id.empty()) return role_permissions(policy, g.role_id);
  return static_cast<uint32_t>(FilePermission::List);
}

/** Grant на подпапку, совпадающую с share-корнем entries (overview → remote). */
bool permissions_from_share_root_grant(const GroupFileAccess& policy,
                                       const std::string& root_norm,
                                       const UserId& user_id,
                                       uint32_t& out) {
  for (const auto& g : policy.root_grants) {
    if (g.relative_path.empty()) continue;
    if (grant_effective_share_root(g) != root_norm) continue;
    if (g.user_id != user_id) continue;
    out = grant_role_permissions(policy, g);
    return true;
  }
  for (const auto& g : policy.root_grants) {
    if (g.relative_path.empty()) continue;
    if (grant_effective_share_root(g) != root_norm) continue;
    if (g.user_id != FileAccessStore::path_role_user()) continue;
    out = grant_role_permissions(policy, g);
    return true;
  }
  return false;
}

const FileRootGrant* find_path_grant(const GroupFileAccess& policy,
                                     const std::string& root_norm,
                                     const std::string& rel_posix,
                                     const UserId& user_id) {
  for (const auto& g : policy.root_grants) {
    if (normalize_grant_root(g.root_path) != root_norm) continue;
    if (path_to_posix_copy(g.relative_path) != rel_posix) continue;
    if (g.user_id != user_id) continue;
    return &g;
  }
  return nullptr;
}

const FileRootGrant* find_wildcard_path_grant(const GroupFileAccess& policy,
                                              const std::string& root_norm,
                                              const std::string& rel_posix) {
  return find_path_grant(policy, root_norm, rel_posix, FileAccessStore::path_role_user());
}

void write_group_policy_json(std::ostream& out, const GroupFileAccess& p) {
  out << "{\"group_id\":\"" << GroupStore::group_id_hex(p.group_id)
      << "\",\"permission_presets\":[";
  for (std::size_t pi = 0; pi < p.permission_presets.size(); ++pi) {
    const auto& preset = p.permission_presets[pi];
    if (pi) out << ',';
    out << "{\"id\":\"" << json_escape(preset.id) << "\",\"name\":\""
        << json_escape(preset.name) << "\",\"permissions\":" << preset.permissions << "}";
  }
  out << "],\"roles\":[";
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
    out << "{\"root\":\"" << json_escape(g.root_path) << "\",\"rel\":\""
        << json_escape(g.relative_path) << "\",\"user_id\":\"" << user_id_hex(g.user_id)
        << "\",\"role_id\":\"" << json_escape(g.role_id) << "\",\"direct\":"
        << g.direct_permissions << ",\"direct_only\":" << (g.direct_only ? "true" : "false")
        << "}";
  }
  out << "]}";
}

bool parse_group_policy_object(const std::string& obj, GroupFileAccess& policy) {
  policy = {};
  if (auto gid = json_get_string(obj, "group_id")) {
    GroupStore::group_id_from_hex(*gid, policy.group_id);
  }
  parse_object_array(obj, "roles", [&](const std::string& role_obj) {
    auto role = parse_role(role_obj);
    if (!role.id.empty()) policy.roles.push_back(std::move(role));
  });
  parse_object_array(obj, "permission_presets", [&](const std::string& p_obj) {
    auto preset = parse_preset(p_obj);
    if (!preset.id.empty()) policy.permission_presets.push_back(std::move(preset));
  });
  parse_object_array(obj, "assignments", [&](const std::string& a_obj) {
    auto a = parse_assignment(a_obj);
    if (!a.role_id.empty()) policy.assignments.push_back(std::move(a));
  });
  parse_object_array(obj, "root_grants", [&](const std::string& g_obj) {
    auto g = parse_root_grant(g_obj);
    if (!g.root_path.empty()) policy.root_grants.push_back(std::move(g));
  });
  return !std::all_of(policy.group_id.begin(), policy.group_id.end(),
                      [](uint8_t b) { return b == 0; });
}

}  // namespace

FileAccessStore::FileAccessStore() { load(); }

std::string FileAccessStore::store_path() { return data_dir() + "/file_access.json"; }

std::string FileAccessStore::role_id_owner() { return "owner"; }
std::string FileAccessStore::role_id_member() { return "member"; }
std::string FileAccessStore::role_id_viewer() { return "viewer"; }

UserId FileAccessStore::path_role_user() { return UserId{}; }

bool FileAccessStore::is_path_role_user(const UserId& user_id) {
  return std::all_of(user_id.begin(), user_id.end(), [](uint8_t b) { return b == 0; });
}

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
  std::ifstream in(store_path(), std::ios::binary);
  if (!in) return true;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  if (json.empty()) return true;

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
          GroupFileAccess policy;
          if (auto gid = json_get_string(obj, "group_id")) {
            GroupStore::group_id_from_hex(*gid, policy.group_id);
          }
          parse_object_array(obj, "roles", [&](const std::string& role_obj) {
            auto role = parse_role(role_obj);
            if (!role.id.empty()) policy.roles.push_back(std::move(role));
          });
          parse_object_array(obj, "permission_presets", [&](const std::string& p_obj) {
            auto preset = parse_preset(p_obj);
            if (!preset.id.empty()) policy.permission_presets.push_back(std::move(preset));
          });
          parse_object_array(obj, "assignments", [&](const std::string& a_obj) {
            auto a = parse_assignment(a_obj);
            if (!a.role_id.empty()) policy.assignments.push_back(std::move(a));
          });
          parse_object_array(obj, "root_grants", [&](const std::string& g_obj) {
            auto g = parse_root_grant(g_obj);
            if (!g.root_path.empty()) policy.root_grants.push_back(std::move(g));
          });
          if (!std::all_of(policy.group_id.begin(), policy.group_id.end(),
                           [](uint8_t b) { return b == 0; })) {
            policies_.push_back(std::move(policy));
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

void FileAccessStore::clear() { policies_.clear(); }

bool FileAccessStore::save() const {
  if (!ensure_data_dir()) return false;
  const std::string path = store_path();
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "{\"groups\":[";
    for (std::size_t gi = 0; gi < policies_.size(); ++gi) {
      const auto& p = policies_[gi];
      if (gi) out << ',';
      out << "{\"group_id\":\"" << GroupStore::group_id_hex(p.group_id)
          << "\",\"permission_presets\":[";
      for (std::size_t pi = 0; pi < p.permission_presets.size(); ++pi) {
        const auto& preset = p.permission_presets[pi];
        if (pi) out << ',';
        out << "{\"id\":\"" << json_escape(preset.id) << "\",\"name\":\""
            << json_escape(preset.name) << "\",\"permissions\":" << preset.permissions << "}";
      }
      out << "],\"roles\":[";
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
        out << "{\"root\":\"" << json_escape(g.root_path) << "\",\"rel\":\""
            << json_escape(g.relative_path) << "\",\"user_id\":\"" << user_id_hex(g.user_id)
            << "\",\"role_id\":\"" << json_escape(g.role_id) << "\",\"direct\":"
            << g.direct_permissions << ",\"direct_only\":" << (g.direct_only ? "true" : "false")
            << "}";
      }
      out << "]}";
    }
    out << "]}\n";
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(tmp, ec);
  }
  return !ec;
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
  save();
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
  return permissions_for(group_id, user_id, root_path, {});
}

uint32_t FileAccessStore::permissions_for(const GroupId& group_id,
                                          const UserId& user_id,
                                          const std::string& root_path,
                                          const std::string& relative_path) const {
  const GroupFileAccess* policy = find_policy(group_id);
  if (!policy) {
    return static_cast<uint32_t>(FilePermission::List) |
           static_cast<uint32_t>(FilePermission::Download);
  }

  const std::string root_norm = root_path.empty() ? std::string{} : normalize_grant_root(root_path);
  const std::string rel_posix = path_to_posix_copy(relative_path);

  if (!root_path.empty()) {
    if (const FileRootGrant* exact = find_path_grant(*policy, root_norm, rel_posix, user_id)) {
      if (exact->direct_only) return exact->direct_permissions;
      if (!exact->role_id.empty()) return role_permissions(*policy, exact->role_id);
    }

    std::string walk = rel_posix;
    while (true) {
      const std::size_t slash = walk.rfind('/');
      walk = slash == std::string::npos ? std::string{} : walk.substr(0, slash);
      if (const FileRootGrant* ancestor = find_path_grant(*policy, root_norm, walk, user_id)) {
        if (ancestor->direct_only) return ancestor->direct_permissions;
        if (!ancestor->role_id.empty()) return role_permissions(*policy, ancestor->role_id);
      }
      if (walk.empty()) break;
    }

    if (const FileRootGrant* path_exact = find_wildcard_path_grant(*policy, root_norm, rel_posix)) {
      if (!path_exact->direct_only && !path_exact->role_id.empty()) {
        return role_permissions(*policy, path_exact->role_id);
      }
    }

    walk = rel_posix;
    while (true) {
      const std::size_t slash = walk.rfind('/');
      walk = slash == std::string::npos ? std::string{} : walk.substr(0, slash);
      if (const FileRootGrant* path_anc = find_wildcard_path_grant(*policy, root_norm, walk)) {
        if (!path_anc->direct_only && !path_anc->role_id.empty()) {
          return role_permissions(*policy, path_anc->role_id);
        }
      }
      if (walk.empty()) break;
    }
  }

  if (!root_path.empty()) {
    uint32_t share_root_perms = 0;
    if (permissions_from_share_root_grant(*policy, root_norm, user_id, share_root_perms)) {
      return share_root_perms;
    }
  }

  std::string role_id = role_id_viewer();
  for (const auto& a : policy->assignments) {
    if (a.user_id == user_id) {
      role_id = a.role_id;
      break;
    }
  }
  return role_permissions(*policy, role_id);
}

bool FileAccessStore::set_path_role(const GroupId& group_id, const std::string& root_path,
                                    const std::string& relative_path,
                                    const std::string& role_id) {
  return set_path_member_role(group_id, root_path, relative_path, path_role_user(), role_id);
}

bool FileAccessStore::set_root_member_role(const GroupId& group_id,
                                           const std::string& root_path,
                                           const UserId& user_id,
                                           const std::string& role_id) {
  return set_path_member_role(group_id, root_path, {}, user_id, role_id);
}

bool FileAccessStore::set_path_member_role(const GroupId& group_id,
                                           const std::string& root_path,
                                           const std::string& relative_path,
                                           const UserId& user_id,
                                           const std::string& role_id) {
  if (root_path.empty()) return false;
  auto& policy = policy_for(group_id);
  const std::string root_norm = normalize_grant_root(root_path);
  const std::string rel_posix = path_to_posix_copy(relative_path);

  if (role_id.empty()) {
    policy.root_grants.erase(
        std::remove_if(policy.root_grants.begin(), policy.root_grants.end(),
                       [&](const FileRootGrant& g) {
                         return normalize_grant_root(g.root_path) == root_norm &&
                                path_to_posix_copy(g.relative_path) == rel_posix &&
                                g.user_id == user_id;
                       }),
        policy.root_grants.end());
    return save();
  }

  for (const auto& r : policy.roles) {
    if (r.id != role_id) continue;
    for (auto& g : policy.root_grants) {
      if (normalize_grant_root(g.root_path) == root_norm &&
          path_to_posix_copy(g.relative_path) == rel_posix && g.user_id == user_id) {
        g.role_id = role_id;
        g.direct_only = false;
        g.direct_permissions = 0;
        return save();
      }
    }
    policy.root_grants.push_back(
        {root_norm, rel_posix, user_id, role_id, 0, false});
    return save();
  }
  return false;
}

bool FileAccessStore::set_path_direct_permissions(const GroupId& group_id,
                                                  const std::string& root_path,
                                                  const std::string& relative_path,
                                                  const UserId& user_id,
                                                  uint32_t permissions) {
  if (root_path.empty()) return false;
  auto& policy = policy_for(group_id);
  const std::string root_norm = normalize_grant_root(root_path);
  const std::string rel_posix = path_to_posix_copy(relative_path);

  if (permissions == 0) {
    policy.root_grants.erase(
        std::remove_if(policy.root_grants.begin(), policy.root_grants.end(),
                       [&](const FileRootGrant& g) {
                         return normalize_grant_root(g.root_path) == root_norm &&
                                path_to_posix_copy(g.relative_path) == rel_posix &&
                                g.user_id == user_id && g.direct_only;
                       }),
        policy.root_grants.end());
    return save();
  }

  for (auto& g : policy.root_grants) {
    if (normalize_grant_root(g.root_path) == root_norm &&
        path_to_posix_copy(g.relative_path) == rel_posix && g.user_id == user_id) {
      g.direct_only = true;
      g.direct_permissions = permissions;
      g.role_id.clear();
      return save();
    }
  }
  FileRootGrant grant;
  grant.root_path = root_norm;
  grant.relative_path = rel_posix;
  grant.user_id = user_id;
  grant.direct_only = true;
  grant.direct_permissions = permissions;
  policy.root_grants.push_back(std::move(grant));
  return save();
}

bool FileAccessStore::upsert_permission_preset(const GroupId& group_id,
                                               const FilePermissionPreset& preset) {
  if (preset.id.empty() || preset.name.empty()) return false;
  auto& policy = policy_for(group_id);
  for (auto& p : policy.permission_presets) {
    if (p.id == preset.id) {
      p.name = preset.name;
      p.permissions = preset.permissions;
      return save();
    }
  }
  policy.permission_presets.push_back(preset);
  return save();
}

bool FileAccessStore::remove_permission_preset(const GroupId& group_id,
                                               const std::string& preset_id) {
  auto& policy = policy_for(group_id);
  const auto it = std::remove_if(policy.permission_presets.begin(), policy.permission_presets.end(),
                                 [&](const FilePermissionPreset& p) { return p.id == preset_id; });
  if (it == policy.permission_presets.end()) return false;
  policy.permission_presets.erase(it, policy.permission_presets.end());
  return save();
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
  for (auto& g : policy.root_grants) {
    if (g.role_id == role_id) g.role_id = role_id_viewer();
  }
  policy.roles.erase(it);
  return save();
}

bool FileAccessStore::has_permission(uint32_t mask, FilePermission perm) {
  return (mask & static_cast<uint32_t>(perm)) != 0;
}

bool FileAccessStore::import_policy(const GroupFileAccess& incoming) {
  if (std::all_of(incoming.group_id.begin(), incoming.group_id.end(),
                  [](uint8_t b) { return b == 0; })) {
    return false;
  }
  for (auto& p : policies_) {
    if (p.group_id != incoming.group_id) continue;
    p = incoming;
    return save();
  }
  policies_.push_back(incoming);
  return save();
}

std::string FileAccessStore::encode_group_policy_json(const GroupFileAccess& policy) {
  std::ostringstream out;
  write_group_policy_json(out, policy);
  return out.str();
}

bool FileAccessStore::decode_group_policy_json(const std::string& json, GroupFileAccess& policy) {
  return parse_group_policy_object(json, policy);
}

}  // namespace nyx
