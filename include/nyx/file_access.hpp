#pragma once

/** @file file_access.hpp
 *  Роли и права доступа к файлам в поле (локальное хранение, фаза 5+).
 */

#include "nyx/chat_id.hpp"
#include "nyx/group.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Битовая маска операций с файлами. */
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

/** Роль с набором прав (настраиваемая или встроенная). */
struct FileRole {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
  bool builtin = false;
};

/** Назначение роли участнику поля. */
struct FileMemberAssignment {
  UserId user_id{};
  std::string role_id;
};

/** Переопределение роли участника для конкретной share-папки. */
struct FileRootGrant {
  std::string root_path;
  UserId user_id{};
  std::string role_id;
};

/** Политика доступа к файлам одного поля. */
struct GroupFileAccess {
  GroupId group_id{};
  std::vector<FileRole> roles;
  std::vector<FileMemberAssignment> assignments;
  std::vector<FileRootGrant> root_grants;
};

/** Хранилище ролей и назначений: data_dir()/file_access.json. */
class FileAccessStore {
 public:
  FileAccessStore();

  bool load();
  bool save() const;

  /** Политика поля; создаёт из roster при первом обращении. */
  GroupFileAccess& ensure_policy(const GroupId& group_id, const GroupRecord& group);

  /** Политика поля; при отсутствии создаёт роли по умолчанию. */
  GroupFileAccess& policy_for(const GroupId& group_id);
  const GroupFileAccess* find_policy(const GroupId& group_id) const;

  /** Права пользователя в поле (0 если не назначен — только List). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id) const;

  /** Права с учётом переопределения на share-папку (root_path UTF-8). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id,
                           const std::string& root_path) const;

  bool set_member_role(const GroupId& group_id, const UserId& user_id,
                       const std::string& role_id);
  /** Роль участника на конкретную share-папку; role_id пустой — снять переопределение. */
  bool set_root_member_role(const GroupId& group_id, const std::string& root_path,
                            const UserId& user_id, const std::string& role_id);
  bool upsert_role(const GroupId& group_id, const FileRole& role);
  bool remove_role(const GroupId& group_id, const std::string& role_id);

  static std::string store_path();
  static std::string role_id_owner();
  static std::string role_id_member();
  static std::string role_id_viewer();
  static GroupFileAccess default_policy(const GroupId& group_id,
                                        const GroupRecord& group);

  static bool has_permission(uint32_t mask, FilePermission perm);

 private:
  std::vector<GroupFileAccess> policies_;
};

}  // namespace nyx
