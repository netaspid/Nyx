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

/** Именованный набор прав (пресет для конструктора). */
struct FilePermissionPreset {
  std::string id;
  std::string name;
  uint32_t permissions = 0;
};

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

/** Права на share-папку, подкаталог или файл внутри корня. */
struct FileRootGrant {
  std::string root_path;
  /** Путь внутри корня (POSIX); пустой — сам share-корень или каталог. */
  std::string relative_path;
  UserId user_id{};
  /** Ненулевая — права роли на этот путь. */
  std::string role_id;
  /** Прямая маска без роли (если direct_only). */
  uint32_t direct_permissions = 0;
  /** true — использовать direct_permissions, role_id игнорируется. */
  bool direct_only = false;
};

/** Политика доступа к файлам одного поля. */
struct GroupFileAccess {
  GroupId group_id{};
  std::vector<FilePermissionPreset> permission_presets;
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
  /** Сбросить политики в памяти (без чтения с диска). */
  void clear();

  /** Политика поля; создаёт из roster при первом обращении. */
  GroupFileAccess& ensure_policy(const GroupId& group_id, const GroupRecord& group);

  /** Политика поля; при отсутствии создаёт роли по умолчанию. */
  GroupFileAccess& policy_for(const GroupId& group_id);
  const GroupFileAccess* find_policy(const GroupId& group_id) const;

  /** Права пользователя в поле (0 если не назначен — только List). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id) const;

  /** Права с учётом share-корня (без относительного пути внутри). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id,
                           const std::string& root_path) const;

  /** Права с учётом корня и относительного пути (файл/подкаталог). */
  uint32_t permissions_for(const GroupId& group_id, const UserId& user_id,
                           const std::string& root_path,
                           const std::string& relative_path) const;

  bool set_member_role(const GroupId& group_id, const UserId& user_id,
                       const std::string& role_id);
  /** Роль участника на share-корень; role_id пустой — снять переопределение на корне. */
  bool set_root_member_role(const GroupId& group_id, const std::string& root_path,
                            const UserId& user_id, const std::string& role_id);
  /** Роль на путь внутри share-корня; role_id пустой — снять grant. */
  bool set_path_member_role(const GroupId& group_id, const std::string& root_path,
                            const std::string& relative_path, const UserId& user_id,
                            const std::string& role_id);
  /** Прямые права на путь без роли; перms=0 — снять grant. */
  bool set_path_direct_permissions(const GroupId& group_id, const std::string& root_path,
                                   const std::string& relative_path, const UserId& user_id,
                                   uint32_t permissions);
  /** Роль на путь для всех участников (user_id = нули); role_id пустой — снять. */
  bool set_path_role(const GroupId& group_id, const std::string& root_path,
                     const std::string& relative_path, const std::string& role_id);
  /** Создать или обновить именованный пресет прав. */
  bool upsert_permission_preset(const GroupId& group_id, const FilePermissionPreset& preset);
  /** Удалить пресет прав по id. */
  bool remove_permission_preset(const GroupId& group_id, const std::string& preset_id);
  bool upsert_role(const GroupId& group_id, const FileRole& role);
  bool remove_role(const GroupId& group_id, const std::string& role_id);

  static std::string store_path();
  static std::string role_id_owner();
  static std::string role_id_member();
  static std::string role_id_viewer();
  /** user_id из нулей — роль на путь для всех участников. */
  static UserId path_role_user();
  static bool is_path_role_user(const UserId& user_id);
  static GroupFileAccess default_policy(const GroupId& group_id,
                                        const GroupRecord& group);

  /** Заменить политику поля (синхронизация с hub). */
  bool import_policy(const GroupFileAccess& policy);

  /** JSON одной политики для PolicyPush. */
  static std::string encode_group_policy_json(const GroupFileAccess& policy);
  /** Разбор JSON политики; false при ошибке. */
  static bool decode_group_policy_json(const std::string& json, GroupFileAccess& policy);

  static bool has_permission(uint32_t mask, FilePermission perm);

 private:
  std::vector<GroupFileAccess> policies_;
};

}  // namespace nyx
