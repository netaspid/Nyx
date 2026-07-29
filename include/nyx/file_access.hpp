#pragma once

/** @file file_access.hpp
 *  File roles and access rights within a field (stored locally).
 */

#include "nyx/chat_id.hpp"
#include "nyx/group.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Bitmask of file operations. */
enum class FilePermission : uint32_t {
  None = 0,
  List = 1u << 0,
  Download = 1u << 1,
  Upload = 1u << 2,
  Delete = 1u << 3,
  OpenRemote = 1u << 4,
  ManageShares = 1u << 5,
  ManageRoles = 1u << 6,
};

constexpr uint32_t kFilePermissionAll =
    static_cast<uint32_t>(FilePermission::List) |
    static_cast<uint32_t>(FilePermission::Download) |
    static_cast<uint32_t>(FilePermission::Upload) |
    static_cast<uint32_t>(FilePermission::Delete) |
    static_cast<uint32_t>(FilePermission::OpenRemote) |
    static_cast<uint32_t>(FilePermission::ManageShares) |
    static_cast<uint32_t>(FilePermission::ManageRoles);

/** Named permission set (a preset for the role editor). */
struct FilePermissionPreset {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
};

/** Role with a permission set (custom or built-in). */
struct FileRole {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
  bool builtin = false;
};

/** Role assignment for a field member. */
struct FileMemberAssignment {
  UserId user_id{};
  std::string role_id;
};

/** Rights on a share folder, subfolder or file inside a root. */
struct FileRootGrant {
  std::string root_path;
  /** Path inside the root (POSIX); empty = the share root itself. */
  std::string relative_path;
  UserId user_id{};
  /** Non-empty = role rights for this path. */
  std::string role_id;
  /** Direct mask without a role (when direct_only). */
  uint32_t direct_permissions = 0;
  /** true = use direct_permissions; role_id is ignored. */
  bool direct_only = false;
};

/** File access policy of one field. */
struct GroupFileAccess {
  GroupId group_id{};
  std::vector<FilePermissionPreset> permission_presets;
  std::vector<FileRole> roles;
  std::vector<FileMemberAssignment> assignments;
  std::vector<FileRootGrant> root_grants;
};

/** Role and assignment storage: data_dir()/file_access.json. */
class FileAccessStore {
 public:
  FileAccessStore();

  bool load();
  bool save() const;
  /** Clears in-memory policies (no disk read). */
  void clear();

  /** Field policy; created from the roster on first access. */
  GroupFileAccess& ensure_policy(const GroupId& group_id, const GroupRecord& group);

  /** Field policy; creates default roles when missing. */
  GroupFileAccess& policy_for(const GroupId& group_id);
  const GroupFileAccess* find_policy(const GroupId& group_id) const;

  /** User rights within the field (0 when unassigned = List only). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id) const;

  /** Rights considering the share root (no relative path inside). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id,
                           const std::string& root_path) const;

  /** Rights considering the root and relative path (file/subfolder). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id,
                           const std::string& root_path,
                           const std::string& relative_path) const;

  bool set_member_role(const GroupId& group_id, const UserId& user_id,
                       const std::string& role_id);
  /** Member role on a share root; empty role_id removes the root override. */
  bool set_root_member_role(const GroupId& group_id, const std::string& root_path,
                            const UserId& user_id, const std::string& role_id);
  /** Role on a path inside a share root; empty role_id removes the grant. */
  bool set_path_member_role(const GroupId& group_id, const std::string& root_path,
                            const std::string& relative_path, const UserId& user_id,
                            const std::string& role_id);
  /** Direct rights on a path without a role; perms=0 removes the grant. */
  bool set_path_direct_permissions(const GroupId& group_id, const std::string& root_path,
                                   const std::string& relative_path, const UserId& user_id,
                                   uint32_t permissions);
  /** Role on a path for all members (user_id = zeros); empty role_id removes it. */
  bool set_path_role(const GroupId& group_id, const std::string& root_path,
                     const std::string& relative_path, const std::string& role_id);
  /** Creates or updates a named permission preset. */
  bool upsert_permission_preset(const GroupId& group_id, const FilePermissionPreset& preset);
  /** Deletes a permission preset by id. */
  bool remove_permission_preset(const GroupId& group_id, const std::string& preset_id);
  bool upsert_role(const GroupId& group_id, const FileRole& role);
  bool remove_role(const GroupId& group_id, const std::string& role_id);

  static std::string store_path();
  static std::string role_id_owner();
  static std::string role_id_member();
  static std::string role_id_viewer();
  /** All-zero user_id = path role for all members. */
  static UserId path_role_user();
  static GroupFileAccess default_policy(const GroupId& group_id,
                                        const GroupRecord& group);

  /** Replaces the field policy (hub synchronization). */
  bool import_policy(const GroupFileAccess& policy);

  /** JSON of one policy for PolicyPush. */
  static std::string encode_group_policy_json(const GroupFileAccess& policy);
  /** Parses a policy JSON; false on error. */
  static bool decode_group_policy_json(const std::string& json, GroupFileAccess& policy);

  static bool has_permission(uint32_t mask, FilePermission perm);

 private:
  std::vector<GroupFileAccess> policies_;
};

}  // namespace nyx
