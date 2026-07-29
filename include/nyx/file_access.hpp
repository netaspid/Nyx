#pragma once

#include "nyx/chat_id.hpp"
#include "nyx/group.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

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
    static_cast<uint32_t>(FilePermission::List) | static_cast<uint32_t>(FilePermission::Download) |
    static_cast<uint32_t>(FilePermission::Upload) | static_cast<uint32_t>(FilePermission::Delete) |
    static_cast<uint32_t>(FilePermission::OpenRemote) |
    static_cast<uint32_t>(FilePermission::ManageShares) |
    static_cast<uint32_t>(FilePermission::ManageRoles);

struct FilePermissionPreset {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
};

struct FileRole {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
  bool builtin = false;
};

struct FileMemberAssignment {
  UserId user_id {};
  std::string role_id;
};

struct FileRootGrant {
  std::string root_path;

  std::string relative_path;
  UserId user_id {};

  std::string role_id;

  uint32_t direct_permissions = 0;

  bool direct_only = false;
};

struct GroupFileAccess {
  GroupId group_id {};
  std::vector<FilePermissionPreset> permission_presets;
  std::vector<FileRole> roles;
  std::vector<FileMemberAssignment> assignments;
  std::vector<FileRootGrant> root_grants;
};

class FileAccessStore {
public:
  FileAccessStore();

  bool load();
  bool save() const;

  void clear();


  GroupFileAccess& ensure_policy(const GroupId& group_id, const GroupRecord& group);


  GroupFileAccess& policy_for(const GroupId& group_id);
  const GroupFileAccess* find_policy(const GroupId& group_id) const;


  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id) const;


  uint32_t permissions_for(const GroupId& group_id,
                           const UserId& user_id,
                           const std::string& root_path) const;


  uint32_t permissions_for(const GroupId& group_id,
                           const UserId& user_id,
                           const std::string& root_path,
                           const std::string& relative_path) const;

  bool set_member_role(const GroupId& group_id, const UserId& user_id, const std::string& role_id);

  bool set_root_member_role(const GroupId& group_id,
                            const std::string& root_path,
                            const UserId& user_id,
                            const std::string& role_id);

  bool set_path_member_role(const GroupId& group_id,
                            const std::string& root_path,
                            const std::string& relative_path,
                            const UserId& user_id,
                            const std::string& role_id);

  bool set_path_direct_permissions(const GroupId& group_id,
                                   const std::string& root_path,
                                   const std::string& relative_path,
                                   const UserId& user_id,
                                   uint32_t permissions);

  bool set_path_role(const GroupId& group_id,
                     const std::string& root_path,
                     const std::string& relative_path,
                     const std::string& role_id);

  bool upsert_permission_preset(const GroupId& group_id, const FilePermissionPreset& preset);

  bool remove_permission_preset(const GroupId& group_id, const std::string& preset_id);
  bool upsert_role(const GroupId& group_id, const FileRole& role);
  bool remove_role(const GroupId& group_id, const std::string& role_id);

  static std::string store_path();
  static std::string role_id_owner();
  static std::string role_id_member();
  static std::string role_id_viewer();

  static UserId path_role_user();
  static GroupFileAccess default_policy(const GroupId& group_id, const GroupRecord& group);


  bool import_policy(const GroupFileAccess& policy);


  static std::string encode_group_policy_json(const GroupFileAccess& policy);

  static bool decode_group_policy_json(const std::string& json, GroupFileAccess& policy);

  static bool has_permission(uint32_t mask, FilePermission perm);

private:
  std::vector<GroupFileAccess> policies_;
};

}
