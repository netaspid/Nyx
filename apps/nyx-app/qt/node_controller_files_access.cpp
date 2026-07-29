#include "node_controller.hpp"

#include "node_controller_media.hpp"

#include "nyx/file_access.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/util.hpp"

#include <QDir>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <string>

namespace {

QString utf8q(const std::string& s) {
  if (s.empty())
    return {};
  const auto n = static_cast<qsizetype>(
      std::min(s.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return QString::fromUtf8(s.data(), n);
}

} // namespace

bool NodeController::canFileList() const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::List));
}

bool NodeController::hasFilePermission(int permissionBit) const {
  return nyx::FileAccessStore::has_permission(currentFilePermissions(),
                                              static_cast<nyx::FilePermission>(permissionBit));
}

bool NodeController::isFileScopeOwner() const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return false;
  const QString scope = files_ui_.file_scope_group_id_.trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString().trimmed().toLower() != scope)
      continue;
    return m.value(QStringLiteral("isOwner")).toBool();
  }
  return false;
}

bool NodeController::canManageFileRoles() const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return false;
  if (isFileScopeOwner())
    return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageRoles));
}

bool NodeController::canFileUpload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Upload));
}

bool NodeController::canFileDownload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Download));
}

bool NodeController::canFileDownloadAt(const QString& rootPath, const QString& relativePath) const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return true;
  if (isFileScopeOwner())
    return true;
  return nyx::FileAccessStore::has_permission(filePermissionsAt(rootPath, relativePath),
                                              nyx::FilePermission::Download);
}

bool NodeController::canDownloadFolderAt(const QString& rootPath,
                                         const QString& relativePath) const {
  if (canFileDownloadAt(rootPath, relativePath))
    return true;
  if (files_ui_.files_section_ == 1 && !files_ui_.file_remote_browse_path_.isEmpty()) {
    return canFileDownloadAt(rootPath, files_ui_.file_remote_browse_path_);
  }
  return false;
}

bool NodeController::canFileOpenRemote() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::OpenRemote));
}

bool NodeController::canFileOpenRemoteAt(const QString& rootPath,
                                         const QString& relativePath) const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return true;
  if (isFileScopeOwner())
    return true;
  return nyx::FileAccessStore::has_permission(filePermissionsAt(rootPath, relativePath),
                                              nyx::FilePermission::OpenRemote);
}

bool NodeController::canManageFileShares() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageShares));
}

bool NodeController::canAddShareFolder() const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageShares)) ||
         hasFilePermission(static_cast<int>(nyx::FilePermission::Upload));
}

void NodeController::refreshFileAccessLists() {
  files_ui_.file_role_list_.clear();
  files_ui_.file_permission_preset_list_.clear();
  files_ui_.file_member_access_.clear();
  if (files_ui_.file_scope_group_id_.isEmpty()) {
    refreshPathRoleState();
    files_ui_.notifyFileAccessChanged();
    return;
  }

  const auto policy = service_.file_access_policy(files_ui_.file_scope_group_id_.toStdString());
  for (const auto& preset : policy.permission_presets) {
    QVariantMap pm;
    pm.insert(QStringLiteral("presetId"), QString::fromStdString(preset.id));
    pm.insert(QStringLiteral("name"), QString::fromStdString(preset.name));
    pm.insert(QStringLiteral("permissions"), static_cast<int>(preset.permissions));
    files_ui_.file_permission_preset_list_.append(pm);
  }
  for (const auto& role : policy.roles) {
    QVariantMap rm;
    rm.insert(QStringLiteral("roleId"), QString::fromStdString(role.id));
    rm.insert(QStringLiteral("name"), QString::fromStdString(role.name));
    rm.insert(QStringLiteral("permissions"), static_cast<int>(role.permissions));
    rm.insert(QStringLiteral("builtin"), role.builtin);
    files_ui_.file_role_list_.append(rm);
  }

  const QString scope = files_ui_.file_scope_group_id_.trimmed().toLower();
  for (const auto& g : service_.list_groups()) {
    const QString gid =
        QString::fromStdString(nyx::GroupStore::group_id_hex(g.id)).trimmed().toLower();
    if (gid != scope)
      continue;
    for (const auto& member : g.members) {
      const QString uid =
          QString::fromStdString(nyx::to_hex(member.user_id.data(), member.user_id.size()));
      QString role_id = QString::fromStdString(nyx::FileAccessStore::role_id_viewer());
      for (const auto& a : policy.assignments) {
        const QString auid =
            QString::fromStdString(nyx::to_hex(a.user_id.data(), a.user_id.size()));
        if (auid == uid) {
          role_id = QString::fromStdString(a.role_id);
          break;
        }
      }
      QString role_name;
      for (const auto& role : policy.roles) {
        if (QString::fromStdString(role.id) == role_id) {
          role_name = QString::fromStdString(role.name);
          break;
        }
      }
      QVariantMap row;
      row.insert(QStringLiteral("userId"), uid);
      row.insert(QStringLiteral("nickname"), QString::fromStdString(member.nickname));
      row.insert(QStringLiteral("idShort"),
                 QString::fromStdString(nyx::short_user_id(member.user_id)));
      row.insert(QStringLiteral("isOwner"), member.role == nyx::GroupRole::Owner);
      row.insert(QStringLiteral("roleId"), role_id);
      row.insert(QStringLiteral("roleName"), role_name);
      files_ui_.file_member_access_.append(row);
    }
    break;
  }

  refreshFilePathMemberAccess();
  refreshPathRoleState();
  files_ui_.notifyFileAccessChanged();
}

void NodeController::refreshFilePathMemberAccess() {
  files_ui_.file_path_member_access_.clear();
  if (files_ui_.file_scope_group_id_.isEmpty() || files_ui_.file_access_target_root_.isEmpty())
    return;

  const auto policy = service_.file_access_policy(files_ui_.file_scope_group_id_.toStdString());
  const std::string root_norm =
      nyx::normalize_grant_root(files_ui_.file_access_target_root_.toStdString());
  const std::string rel_posix = files_ui_.file_access_target_rel_.toStdString();
  auto rel_posix_norm = rel_posix;
  for (char& c : rel_posix_norm) {
    if (c == '\\')
      c = '/';
  }

  auto find_grant = [&](const std::string& rel,
                        const nyx::UserId& user) -> const nyx::FileRootGrant* {
    for (const auto& g : policy.root_grants) {
      if (nyx::normalize_grant_root(g.root_path) != root_norm)
        continue;
      std::string gr = g.relative_path;
      for (char& c : gr) {
        if (c == '\\')
          c = '/';
      }
      if (gr != rel)
        continue;
      if (g.user_id != user)
        continue;
      return &g;
    }
    return nullptr;
  };

  for (const QVariant& mv : files_ui_.file_member_access_) {
    const QVariantMap mm = mv.toMap();
    if (mm.value(QStringLiteral("isOwner")).toBool())
      continue;
    const QString uid = mm.value(QStringLiteral("userId")).toString();
    nyx::UserId user {};
    nyx::ByteBuffer buf;
    if (!nyx::from_hex(uid.toStdString(), buf) || buf.size() != user.size())
      continue;
    std::memcpy(user.data(), buf.data(), buf.size());

    QString grantMode = QStringLiteral("inherit");
    QString role_id;
    int direct_perms = 0;
    QString inherited_from;

    if (const nyx::FileRootGrant* exact = find_grant(rel_posix_norm, user)) {
      if (exact->direct_only) {
        grantMode = QStringLiteral("direct");
        direct_perms = static_cast<int>(exact->direct_permissions);
      } else if (!exact->role_id.empty()) {
        grantMode = QStringLiteral("role");
        role_id = utf8q(exact->role_id);
      }
    }

    std::string walk = rel_posix_norm;
    while (grantMode == QStringLiteral("inherit")) {
      const std::size_t slash = walk.rfind('/');
      walk = slash == std::string::npos ? std::string {} : walk.substr(0, slash);
      if (const nyx::FileRootGrant* anc = find_grant(walk, user)) {
        if (!anc->direct_only && !anc->role_id.empty()) {
          grantMode = QStringLiteral("inherited");
          role_id = utf8q(anc->role_id);
          inherited_from = walk.empty() ? QStringLiteral("корень") : utf8q(walk);
          break;
        }
      }
      if (walk.empty())
        break;
    }

    QVariantMap row = mm;
    row.insert(QStringLiteral("grantMode"), grantMode);
    row.insert(QStringLiteral("roleId"), role_id);
    row.insert(QStringLiteral("directPermissions"), direct_perms);
    row.insert(QStringLiteral("inheritedFrom"), inherited_from);
    files_ui_.file_path_member_access_.append(row);
  }
}

void NodeController::refreshPathRoleState() {
  files_ui_.file_path_role_id_.clear();
  files_ui_.file_path_role_inherited_from_.clear();
  if (files_ui_.file_scope_group_id_.isEmpty() || files_ui_.file_access_target_root_.isEmpty())
    return;

  const auto policy = service_.file_access_policy(files_ui_.file_scope_group_id_.toStdString());
  const std::string root_norm =
      nyx::normalize_grant_root(files_ui_.file_access_target_root_.toStdString());
  std::string rel_posix_norm = files_ui_.file_access_target_rel_.toStdString();
  for (char& c : rel_posix_norm) {
    if (c == '\\')
      c = '/';
  }

  const nyx::UserId wildcard = nyx::FileAccessStore::path_role_user();
  auto find_wildcard = [&](const std::string& rel) -> const nyx::FileRootGrant* {
    for (const auto& g : policy.root_grants) {
      if (nyx::normalize_grant_root(g.root_path) != root_norm)
        continue;
      std::string gr = g.relative_path;
      for (char& c : gr) {
        if (c == '\\')
          c = '/';
      }
      if (gr != rel)
        continue;
      if (g.user_id != wildcard)
        continue;
      return &g;
    }
    return nullptr;
  };

  if (const nyx::FileRootGrant* exact = find_wildcard(rel_posix_norm)) {
    if (!exact->direct_only && !exact->role_id.empty()) {
      files_ui_.file_path_role_id_ = utf8q(exact->role_id);
      return;
    }
  }

  std::string walk = rel_posix_norm;
  while (true) {
    const std::size_t slash = walk.rfind('/');
    walk = slash == std::string::npos ? std::string {} : walk.substr(0, slash);
    if (const nyx::FileRootGrant* anc = find_wildcard(walk)) {
      if (!anc->direct_only && !anc->role_id.empty()) {
        files_ui_.file_path_role_id_ = utf8q(anc->role_id);
        files_ui_.file_path_role_inherited_from_ =
            walk.empty() ? QStringLiteral("корень") : utf8q(walk);
        return;
      }
    }
    if (walk.empty())
      break;
  }
}

void NodeController::updateFileAccessTargetLabel() {
  if (files_ui_.file_access_target_root_.isEmpty()) {
    files_ui_.file_access_target_label_.clear();
    return;
  }
  const QFileInfo rootInfo(files_ui_.file_access_target_root_);
  QString label =
      rootInfo.fileName().isEmpty() ? files_ui_.file_access_target_root_ : rootInfo.fileName();
  if (!files_ui_.file_access_target_rel_.isEmpty()) {
    label += QStringLiteral(" / ") + files_ui_.file_access_target_rel_;
  }
  files_ui_.file_access_target_label_ = label;
}

void NodeController::setFileAccessTarget(const QString& rootPath, const QString& relativePath) {
  files_ui_.file_access_target_root_ = resolveAccessRootPath(rootPath);
  files_ui_.file_access_target_rel_ = relativePath.trimmed();
  files_ui_.file_access_target_rel_.replace(QLatin1Char('\\'), QLatin1Char('/'));
  updateFileAccessTargetLabel();
  refreshFilePathMemberAccess();
  files_ui_.notifyFileAccessChanged();
}

QString NodeController::resolveAccessRootPath(const QString& rootPath) const {
  QString p = rootPath.trimmed();
  if (p.startsWith(QStringLiteral("file:///")))
    p = QUrl(p).toLocalFile();
  if (p.isEmpty())
    return p;
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(p, QString::fromStdString(r.path))) {
      return QString::fromStdString(r.path);
    }
  }
  return normalizeShareRootPath(p);
}

bool NodeController::canEditFileRolePermissions(const QString& roleId) const {
  return roleId.trimmed().toLower() !=
         QString::fromStdString(nyx::FileAccessStore::role_id_owner());
}

void NodeController::setPathMemberFileRole(const QString& userIdHex, const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"), true);
    return;
  }
  if (files_ui_.file_access_target_root_.isEmpty()) {
    showToast(QStringLiteral("Выберите объект для назначения прав"), true);
    return;
  }
  if (!service_.set_path_member_file_role(files_ui_.file_scope_group_id_.toStdString(),
                                          files_ui_.file_access_target_root_.toStdString(),
                                          files_ui_.file_access_target_rel_.toStdString(),
                                          userIdHex.toStdString(),
                                          roleId.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Права на объект обновлены"));
}

void NodeController::setPathGrantDirect(const QString& userIdHex) {
  if (!canManageFileRoles())
    return;
  const int perms =
      static_cast<int>(nyx::FilePermission::List) | static_cast<int>(nyx::FilePermission::Download);
  if (!service_.set_path_direct_file_permissions(files_ui_.file_scope_group_id_.toStdString(),
                                                 files_ui_.file_access_target_root_.toStdString(),
                                                 files_ui_.file_access_target_rel_.toStdString(),
                                                 userIdHex.toStdString(),
                                                 static_cast<uint32_t>(perms))) {
    showToast(QStringLiteral("Не удалось задать прямые права"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::clearPathMemberGrant(const QString& userIdHex) {
  if (!canManageFileRoles())
    return;
  service_.set_path_member_file_role(files_ui_.file_scope_group_id_.toStdString(),
                                     files_ui_.file_access_target_root_.toStdString(),
                                     files_ui_.file_access_target_rel_.toStdString(),
                                     userIdHex.toStdString(),
                                     {});
  refreshFileAccessLists();
}

void NodeController::setPathRole(const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"), true);
    return;
  }
  if (files_ui_.file_access_target_root_.isEmpty()) {
    showToast(QStringLiteral("Выберите объект"), true);
    return;
  }
  if (!service_.set_path_role(files_ui_.file_scope_group_id_.toStdString(),
                              files_ui_.file_access_target_root_.toStdString(),
                              files_ui_.file_access_target_rel_.toStdString(),
                              roleId.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Роль на объект обновлена"));
}

void NodeController::clearPathRole() {
  setPathRole({});
}

void NodeController::createPermissionPreset(const QString& name, int permissions) {
  if (!canManageFileRoles())
    return;
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty())
    return;
  nyx::FilePermissionPreset preset;
  uint8_t id_bytes[4];
  nyx::random_bytes(id_bytes, sizeof(id_bytes));
  preset.id = "preset_" + nyx::to_hex(id_bytes, sizeof(id_bytes));
  preset.name = trimmed.toStdString();
  preset.permissions = static_cast<uint32_t>(permissions);
  if (!service_.upsert_permission_preset(files_ui_.file_scope_group_id_.toStdString(), preset)) {
    showToast(QStringLiteral("Не удалось создать пресет"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Пресет прав создан"));
}

void NodeController::deletePermissionPreset(const QString& presetId) {
  if (!canManageFileRoles())
    return;
  if (!service_.remove_permission_preset(files_ui_.file_scope_group_id_.toStdString(),
                                         presetId.toStdString())) {
    showToast(QStringLiteral("Не удалось удалить пресет"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::togglePermissionPresetBit(const QString& presetId, int permissionBit) {
  if (!canManageFileRoles())
    return;
  for (const QVariant& pv : files_ui_.file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId)
      continue;
    int perms = pm.value(QStringLiteral("permissions")).toInt();
    perms ^= permissionBit;
    nyx::FilePermissionPreset preset;
    preset.id = presetId.toStdString();
    preset.name = pm.value(QStringLiteral("name")).toString().toStdString();
    preset.permissions = static_cast<uint32_t>(perms);
    service_.upsert_permission_preset(files_ui_.file_scope_group_id_.toStdString(), preset);
    refreshFileAccessLists();
    return;
  }
}

void NodeController::applyPresetToRole(const QString& presetId, const QString& roleId) {
  if (!canManageFileRoles() || !canEditFileRolePermissions(roleId))
    return;
  int perms = 0;
  QString preset_name;
  for (const QVariant& pv : files_ui_.file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId)
      continue;
    perms = pm.value(QStringLiteral("permissions")).toInt();
    preset_name = pm.value(QStringLiteral("name")).toString();
    break;
  }
  for (const QVariant& rv : files_ui_.file_role_list_) {
    const QVariantMap rm = rv.toMap();
    if (rm.value(QStringLiteral("roleId")).toString() != roleId)
      continue;
    updateFileRole(roleId, rm.value(QStringLiteral("name")).toString(), perms);
    showToast(QStringLiteral("К роли применён пресет «") + preset_name + QStringLiteral("»"));
    return;
  }
}

void NodeController::togglePathDirectPermission(const QString& userIdHex, int permissionBit) {
  if (!canManageFileRoles())
    return;
  int perms =
      static_cast<int>(nyx::FilePermission::List) | static_cast<int>(nyx::FilePermission::Download);
  for (const QVariant& pv : files_ui_.file_path_member_access_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("userId")).toString() != userIdHex)
      continue;
    if (pm.value(QStringLiteral("grantMode")).toString() == QStringLiteral("direct")) {
      perms = pm.value(QStringLiteral("directPermissions")).toInt();
    }
    break;
  }
  perms ^= permissionBit;
  if (!service_.set_path_direct_file_permissions(files_ui_.file_scope_group_id_.toStdString(),
                                                 files_ui_.file_access_target_root_.toStdString(),
                                                 files_ui_.file_access_target_rel_.toStdString(),
                                                 userIdHex.toStdString(),
                                                 static_cast<uint32_t>(perms))) {
    showToast(QStringLiteral("Не удалось обновить права"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::refreshFieldRoster() {
  refreshGroupList();
  if (field_info_open_) {
    syncFieldInfoState();
    emit fieldInfoOpenChanged();
  }
  refreshFileAccessLists();
}

void NodeController::setMemberFileRole(const QString& userIdHex, const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  if (!service_.set_member_file_role(files_ui_.file_scope_group_id_.toStdString(),
                                     userIdHex.toStdString(),
                                     roleId.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль"));
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Роль обновлена"));
}

void NodeController::createFileRole(const QString& name, int permissions) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  nyx::FileRole role;
  role.id = "role_" + std::to_string(QDateTime::currentMSecsSinceEpoch());
  role.name = name.trimmed().toStdString();
  role.permissions = static_cast<uint32_t>(permissions);
  if (role.name.empty() ||
      !service_.upsert_file_role(files_ui_.file_scope_group_id_.toStdString(), role)) {
    showToast(QStringLiteral("Не удалось создать роль"));
    return;
  }
  refreshFileAccessLists();
}

void NodeController::updateFileRole(const QString& roleId, const QString& name, int permissions) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  nyx::FileRole role;
  role.id = roleId.toStdString();
  role.name = name.trimmed().toStdString();
  role.permissions = static_cast<uint32_t>(permissions);
  if (!service_.upsert_file_role(files_ui_.file_scope_group_id_.toStdString(), role)) {
    showToast(QStringLiteral("Не удалось обновить роль"));
    return;
  }
  refreshFileAccessLists();
  if (files_ui_.files_section_ == 1)
    refreshRemoteFileModel();
  files_ui_.notifyFilesChanged();
}

void NodeController::deleteFileRole(const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  if (!service_.remove_file_role(files_ui_.file_scope_group_id_.toStdString(),
                                 roleId.toStdString())) {
    showToast(QStringLiteral("Нельзя удалить встроенную роль"));
    return;
  }
  refreshFileAccessLists();
}

void NodeController::toggleFileRolePermission(const QString& roleId, int permissionBit) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"), true);
    return;
  }
  if (!canEditFileRolePermissions(roleId)) {
    showToast(QStringLiteral("Роль владельца нельзя менять"), true);
    return;
  }
  for (const QVariant& rv : files_ui_.file_role_list_) {
    const QVariantMap rm = rv.toMap();
    if (rm.value(QStringLiteral("roleId")).toString() != roleId)
      continue;
    int perms = rm.value(QStringLiteral("permissions")).toInt();
    perms ^= permissionBit;
    updateFileRole(roleId, rm.value(QStringLiteral("name")).toString(), perms);
    return;
  }
}

void NodeController::openRemoteFile(const QString& hashHex,
                                    const QString& fileName,
                                    const QString& rootPath,
                                    const QString& relativePath) {
  if (!canFileOpenRemoteAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права открывать файлы по сети"));
    return;
  }
  openFileByHash(hashHex, fileName, {}, rootPath, relativePath);
}

void NodeController::openFilesView() {
  setMainViewMode(1);
}

void NodeController::openChatMediaFolder(const QString& mediaKind) {
  const QString kind = mediaKind.trimmed().toLower();
  if (kind != QLatin1String("voice") && kind != QLatin1String("circle"))
    return;

  QString scope_id;
  nyx::GroupId scope {};
  if (active_chat_kind_ == 1) {
    scope_id = active_chat_ref_id_.trimmed().toLower();
    if (!nyx::GroupStore::group_id_from_hex(scope_id.toStdString(), scope)) {
      showToast(QStringLiteral("Не удалось определить хранилище чата"));
      return;
    }
  }

  setFileScopeGroupId(scope_id);
  setFilesSection(0);
  refreshFileShareRoots();

  const QString root = QString::fromStdString(nyx::FileIndex::library_root_path(scope));
  setFileSelectedShareRoot(root);
  const QString relative = nyxMediaRelativeDir(active_chat_key_, peer_title_, kind);
  const QString directory = QDir(root).filePath(relative);
  if (!QDir(directory).exists()) {
    setMainViewMode(1);
    showToast(QStringLiteral("В этом чате пока нет сохранённых медиа"));
    return;
  }

  browseIntoFolder(relative);
  setMainViewMode(1);
}

void NodeController::showChatView() {
  setMainViewMode(0);
}

uint32_t NodeController::filePermissionsAt(const QString& rootPath,
                                           const QString& relativePath) const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return nyx::kFilePermissionAll;
  if (isFileScopeOwner())
    return nyx::kFilePermissionAll;
  const QString root = resolveAccessRootPath(rootPath);
  return service_.my_file_permissions(
      files_ui_.file_scope_group_id_.toStdString(), root.toStdString(), relativePath.toStdString());
}

uint32_t NodeController::currentFilePermissions() const {
  if (files_ui_.file_scope_group_id_.isEmpty())
    return nyx::kFilePermissionAll;
  if (isFileScopeOwner())
    return nyx::kFilePermissionAll;

  QString root;
  QString rel;
  if (files_ui_.files_section_ == 1) {
    root = files_ui_.file_resources_root_;
    rel = files_ui_.file_remote_browse_path_;
  } else if (files_ui_.files_section_ == 0) {
    root = files_ui_.file_selected_share_root_;
    rel = files_ui_.file_browse_path_;
  }
  return filePermissionsAt(root, rel);
}
