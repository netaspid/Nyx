#include "node_controller.hpp"

#include "android_platform.hpp"
#include "host_env.hpp"
#include "node_controller_media.hpp"

#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace {

QString formatFileSizeLabel(quint64 bytes, bool is_directory) {
  if (is_directory) {
    if (bytes == 0)
      return QStringLiteral("пустая папка");
    if (bytes == 1)
      return QStringLiteral("1 файл");
    return QStringLiteral("%1 файлов").arg(bytes);
  }
  if (bytes < 1024)
    return QString::number(bytes) + QStringLiteral(" B");
  if (bytes < 1024 * 1024)
    return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
  if (bytes < 1024ULL * 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
  }
  return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + QStringLiteral(" GB");
}

QString utf8q(const std::string& s) {
  if (s.empty())
    return {};
  const auto n = static_cast<qsizetype>(
      std::min(s.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return QString::fromUtf8(s.data(), n);
}

} // namespace

QString NodeController::joinFileRelPath(const QString& browseRel, const QString& entryRel) const {
  QString rel = entryRel.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const QString browse = browseRel.trimmed();
  if (browse.isEmpty())
    return rel;
  if (rel.isEmpty())
    return browse;
  if (rel.startsWith(browse + QLatin1Char('/')))
    return rel;
  return browse + QLatin1Char('/') + rel;
}

QVariantList NodeController::entriesToVariant(const std::vector<nyx::FileEntry>& entries,
                                              bool remote) const {
  QVariantList list;
  const QString browse_rel = remote ? file_remote_browse_path_ : file_browse_path_;
  for (const auto& e : entries) {
    QVariantMap m;
    const std::string leaf = e.leaf_name();
    const std::string rel = e.relative_path;
    QString display = utf8q(leaf);
    if (display.isEmpty() && !rel.empty())
      display = utf8q(rel);
    const bool is_dir = e.is_directory();
    QString full_rel;
    if (is_dir) {
      full_rel = utf8q(rel);
      full_rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    } else {
      full_rel = joinFileRelPath(browse_rel, utf8q(rel));
    }
    const QString root = utf8q(e.root_path);
    QString owner_hex;
    if (!std::all_of(e.owner_id.begin(), e.owner_id.end(), [](uint8_t b) { return b == 0; })) {
      owner_hex = QString::fromStdString(nyx::to_hex(e.owner_id.data(), e.owner_id.size()));
    } else if (is_dir && display.size() == 64) {

      bool hex_ok = true;
      for (const QChar c : display) {
        if (!c.isDigit() && (c.toLower() < QLatin1Char('a') || c.toLower() > QLatin1Char('f'))) {
          hex_ok = false;
          break;
        }
      }
      if (hex_ok)
        owner_hex = display.toLower();
    } else {
      const QString posix = full_rel;
      const int slash = posix.indexOf(QLatin1Char('/'));
      const QString head = slash > 0 ? posix.left(slash) : QString {};
      if (head.size() == 64)
        owner_hex = head.toLower();
    }
    QString owner_label;
    if (!owner_hex.isEmpty()) {
      owner_label = userDisplayName(owner_hex);
      if (is_dir && display.toLower() == owner_hex)
        display = owner_label;
    }
    m.insert(QStringLiteral("name"), display);
    m.insert(QStringLiteral("navPath"), utf8q(rel));
    m.insert(QStringLiteral("fullRelPath"), full_rel);
    m.insert(QStringLiteral("rootPath"), root);
    m.insert(QStringLiteral("hash"), utf8q(nyx::hash_hex(e.hash)));
    const auto size = static_cast<qulonglong>(e.size);
    m.insert(QStringLiteral("size"), size);
    m.insert(QStringLiteral("sizeLabel"), formatFileSizeLabel(size, is_dir));
    m.insert(QStringLiteral("mime"), utf8q(e.mime));
    m.insert(QStringLiteral("isRemote"), remote);
    m.insert(QStringLiteral("isDirectory"), is_dir);
    m.insert(QStringLiteral("ownerId"), owner_hex);
    m.insert(QStringLiteral("ownerLabel"), owner_label);
    if (remote) {
      const bool can_dl =
          is_dir ? canDownloadFolderAt(root, full_rel) : canFileDownloadAt(root, full_rel);
      m.insert(QStringLiteral("canDownload"), can_dl);
      m.insert(QStringLiteral("canOpenRemote"), !is_dir && canFileOpenRemoteAt(root, full_rel));
    }
    list.append(m);
  }
  return list;
}

void NodeController::resetFileBrowse() {
  file_browse_path_.clear();
  syncFileBrowseCrumbs();
}

QString NodeController::normalizeShareRootPath(const QString& path) const {
  QString p = path.trimmed();
  if (p.isEmpty())
    return p;
  p.replace(QLatin1Char('\\'), QLatin1Char('/'));
#ifdef Q_OS_WIN
  return p.toLower();
#else
  return p;
#endif
}

bool NodeController::shareRootPathsEqual(const QString& a, const QString& b) const {
  return normalizeShareRootPath(a) == normalizeShareRootPath(b);
}

bool NodeController::canFileList() const {
  if (file_scope_group_id_.isEmpty())
    return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::List));
}

bool NodeController::hasFilePermission(int permissionBit) const {
  return nyx::FileAccessStore::has_permission(currentFilePermissions(),
                                              static_cast<nyx::FilePermission>(permissionBit));
}

bool NodeController::isFileScopeOwner() const {
  if (file_scope_group_id_.isEmpty())
    return false;
  const QString scope = file_scope_group_id_.trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString().trimmed().toLower() != scope)
      continue;
    return m.value(QStringLiteral("isOwner")).toBool();
  }
  return false;
}

bool NodeController::canManageFileRoles() const {
  if (file_scope_group_id_.isEmpty())
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
  if (file_scope_group_id_.isEmpty())
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
  if (files_section_ == 1 && !file_remote_browse_path_.isEmpty()) {
    return canFileDownloadAt(rootPath, file_remote_browse_path_);
  }
  return false;
}

bool NodeController::canFileOpenRemote() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::OpenRemote));
}

bool NodeController::canFileOpenRemoteAt(const QString& rootPath,
                                         const QString& relativePath) const {
  if (file_scope_group_id_.isEmpty())
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
  if (file_scope_group_id_.isEmpty())
    return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageShares)) ||
         hasFilePermission(static_cast<int>(nyx::FilePermission::Upload));
}

void NodeController::refreshFileAccessLists() {
  file_role_list_.clear();
  file_permission_preset_list_.clear();
  file_member_access_.clear();
  if (file_scope_group_id_.isEmpty()) {
    refreshPathRoleState();
    emit fileAccessChanged();
    return;
  }

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  for (const auto& preset : policy.permission_presets) {
    QVariantMap pm;
    pm.insert(QStringLiteral("presetId"), QString::fromStdString(preset.id));
    pm.insert(QStringLiteral("name"), QString::fromStdString(preset.name));
    pm.insert(QStringLiteral("permissions"), static_cast<int>(preset.permissions));
    file_permission_preset_list_.append(pm);
  }
  for (const auto& role : policy.roles) {
    QVariantMap rm;
    rm.insert(QStringLiteral("roleId"), QString::fromStdString(role.id));
    rm.insert(QStringLiteral("name"), QString::fromStdString(role.name));
    rm.insert(QStringLiteral("permissions"), static_cast<int>(role.permissions));
    rm.insert(QStringLiteral("builtin"), role.builtin);
    file_role_list_.append(rm);
  }

  const QString scope = file_scope_group_id_.trimmed().toLower();
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
      file_member_access_.append(row);
    }
    break;
  }

  refreshFilePathMemberAccess();
  refreshPathRoleState();
  emit fileAccessChanged();
}

void NodeController::refreshFilePathMemberAccess() {
  file_path_member_access_.clear();
  if (file_scope_group_id_.isEmpty() || file_access_target_root_.isEmpty())
    return;

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  const std::string root_norm = nyx::normalize_grant_root(file_access_target_root_.toStdString());
  const std::string rel_posix = file_access_target_rel_.toStdString();
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

  for (const QVariant& mv : file_member_access_) {
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
    file_path_member_access_.append(row);
  }
}

void NodeController::refreshPathRoleState() {
  file_path_role_id_.clear();
  file_path_role_inherited_from_.clear();
  if (file_scope_group_id_.isEmpty() || file_access_target_root_.isEmpty())
    return;

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  const std::string root_norm = nyx::normalize_grant_root(file_access_target_root_.toStdString());
  std::string rel_posix_norm = file_access_target_rel_.toStdString();
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
      file_path_role_id_ = utf8q(exact->role_id);
      return;
    }
  }

  std::string walk = rel_posix_norm;
  while (true) {
    const std::size_t slash = walk.rfind('/');
    walk = slash == std::string::npos ? std::string {} : walk.substr(0, slash);
    if (const nyx::FileRootGrant* anc = find_wildcard(walk)) {
      if (!anc->direct_only && !anc->role_id.empty()) {
        file_path_role_id_ = utf8q(anc->role_id);
        file_path_role_inherited_from_ = walk.empty() ? QStringLiteral("корень") : utf8q(walk);
        return;
      }
    }
    if (walk.empty())
      break;
  }
}

void NodeController::updateFileAccessTargetLabel() {
  if (file_access_target_root_.isEmpty()) {
    file_access_target_label_.clear();
    return;
  }
  const QFileInfo rootInfo(file_access_target_root_);
  QString label = rootInfo.fileName().isEmpty() ? file_access_target_root_ : rootInfo.fileName();
  if (!file_access_target_rel_.isEmpty()) {
    label += QStringLiteral(" / ") + file_access_target_rel_;
  }
  file_access_target_label_ = label;
}

void NodeController::setFileAccessTarget(const QString& rootPath, const QString& relativePath) {
  file_access_target_root_ = resolveAccessRootPath(rootPath);
  file_access_target_rel_ = relativePath.trimmed();
  file_access_target_rel_.replace(QLatin1Char('\\'), QLatin1Char('/'));
  updateFileAccessTargetLabel();
  refreshFilePathMemberAccess();
  emit fileAccessChanged();
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
  if (file_access_target_root_.isEmpty()) {
    showToast(QStringLiteral("Выберите объект для назначения прав"), true);
    return;
  }
  if (!service_.set_path_member_file_role(file_scope_group_id_.toStdString(),
                                          file_access_target_root_.toStdString(),
                                          file_access_target_rel_.toStdString(),
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
  if (!service_.set_path_direct_file_permissions(file_scope_group_id_.toStdString(),
                                                 file_access_target_root_.toStdString(),
                                                 file_access_target_rel_.toStdString(),
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
  service_.set_path_member_file_role(file_scope_group_id_.toStdString(),
                                     file_access_target_root_.toStdString(),
                                     file_access_target_rel_.toStdString(),
                                     userIdHex.toStdString(),
                                     {});
  refreshFileAccessLists();
}

void NodeController::setPathRole(const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"), true);
    return;
  }
  if (file_access_target_root_.isEmpty()) {
    showToast(QStringLiteral("Выберите объект"), true);
    return;
  }
  if (!service_.set_path_role(file_scope_group_id_.toStdString(),
                              file_access_target_root_.toStdString(),
                              file_access_target_rel_.toStdString(),
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
  if (!service_.upsert_permission_preset(file_scope_group_id_.toStdString(), preset)) {
    showToast(QStringLiteral("Не удалось создать пресет"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Пресет прав создан"));
}

void NodeController::deletePermissionPreset(const QString& presetId) {
  if (!canManageFileRoles())
    return;
  if (!service_.remove_permission_preset(file_scope_group_id_.toStdString(),
                                         presetId.toStdString())) {
    showToast(QStringLiteral("Не удалось удалить пресет"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::togglePermissionPresetBit(const QString& presetId, int permissionBit) {
  if (!canManageFileRoles())
    return;
  for (const QVariant& pv : file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId)
      continue;
    int perms = pm.value(QStringLiteral("permissions")).toInt();
    perms ^= permissionBit;
    nyx::FilePermissionPreset preset;
    preset.id = presetId.toStdString();
    preset.name = pm.value(QStringLiteral("name")).toString().toStdString();
    preset.permissions = static_cast<uint32_t>(perms);
    service_.upsert_permission_preset(file_scope_group_id_.toStdString(), preset);
    refreshFileAccessLists();
    return;
  }
}

void NodeController::applyPresetToRole(const QString& presetId, const QString& roleId) {
  if (!canManageFileRoles() || !canEditFileRolePermissions(roleId))
    return;
  int perms = 0;
  QString preset_name;
  for (const QVariant& pv : file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId)
      continue;
    perms = pm.value(QStringLiteral("permissions")).toInt();
    preset_name = pm.value(QStringLiteral("name")).toString();
    break;
  }
  for (const QVariant& rv : file_role_list_) {
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
  for (const QVariant& pv : file_path_member_access_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("userId")).toString() != userIdHex)
      continue;
    if (pm.value(QStringLiteral("grantMode")).toString() == QStringLiteral("direct")) {
      perms = pm.value(QStringLiteral("directPermissions")).toInt();
    }
    break;
  }
  perms ^= permissionBit;
  if (!service_.set_path_direct_file_permissions(file_scope_group_id_.toStdString(),
                                                 file_access_target_root_.toStdString(),
                                                 file_access_target_rel_.toStdString(),
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
  if (!service_.set_member_file_role(
          file_scope_group_id_.toStdString(), userIdHex.toStdString(), roleId.toStdString())) {
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
  if (role.name.empty() || !service_.upsert_file_role(file_scope_group_id_.toStdString(), role)) {
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
  if (!service_.upsert_file_role(file_scope_group_id_.toStdString(), role)) {
    showToast(QStringLiteral("Не удалось обновить роль"));
    return;
  }
  refreshFileAccessLists();
  if (files_section_ == 1)
    refreshRemoteFileModel();
  emit filesChanged();
}

void NodeController::deleteFileRole(const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  if (!service_.remove_file_role(file_scope_group_id_.toStdString(), roleId.toStdString())) {
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
  for (const QVariant& rv : file_role_list_) {
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

void NodeController::setFileScopeGroupId(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (file_scope_group_id_ == gid)
    return;
  file_scope_group_id_ = gid;
  file_selected_share_root_.clear();
  resetFileBrowse();
  syncFileScopeLabel();
  service_.save_files_scope_group_id(gid.toStdString());
  service_.save_files_selected_root({});
  if (!gid.isEmpty()) {
    service_.set_active_session(QStringLiteral("group:%1").arg(gid).toStdString());
  }
  refreshFileLists();
  refreshFileAccessLists();
  if (fileExchangeReady())
    refreshRemoteFileList();
  emit filesChanged();
  emit fileAccessChanged();
}

bool NodeController::fileExchangeReady() const {
  if (!file_scope_group_id_.isEmpty()) {
    return !service_.file_exchange_session_id(file_scope_group_id_.toStdString()).empty();
  }
  return service_.can_request_remote_files();
}

QString NodeController::fileExchangeHint() const {
  return QString::fromStdString(service_.file_exchange_hint());
}

QString NodeController::scopeLabelForGroupId(const QString& groupIdHex) const {
  if (groupIdHex.isEmpty())
    return QStringLiteral("Личные");
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == groupIdHex) {
      return m.value(QStringLiteral("name")).toString();
    }
  }
  return groupIdHex.left(8) + QStringLiteral("…");
}

bool NodeController::canRemoveShareRoot(const nyx::ShareRoot& root) const {
  if (root.is_personal())
    return true;
  const QString gid =
      QString::fromStdString(nyx::FileIndex::group_id_hex(root.group_id)).trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid)
      continue;
    if (m.value(QStringLiteral("isOwner")).toBool())
      return true;
    break;
  }
  const uint32_t perms = service_.my_file_permissions(gid.toStdString(), {});
  return nyx::FileAccessStore::has_permission(perms, nyx::FilePermission::ManageShares);
}

void NodeController::resetFilesUiState() {
  file_scope_group_id_.clear();
  file_selected_share_root_.clear();
  file_share_roots_.clear();
  file_role_list_.clear();
  file_member_access_.clear();
  file_browse_path_.clear();
  file_browse_crumbs_.clear();
  file_resources_root_.clear();
  file_remote_browse_path_.clear();
  file_remote_browse_crumbs_.clear();
  local_file_list_.clear();
  remote_file_list_.clear();
  file_scope_label_ = QStringLiteral("Личные файлы");
}

void NodeController::syncFileScopeFromSavedOrRoots() {
  const auto roots = service_.all_share_roots();
  if (roots.empty())
    return;

  if (!file_selected_share_root_.isEmpty()) {
    const auto scope_roots = service_.share_roots_for_scope(file_scope_group_id_.toStdString());
    for (const auto& r : scope_roots) {
      const QString path = QString::fromStdString(r.path);
      if (!shareRootPathsEqual(file_selected_share_root_, path))
        continue;
      file_selected_share_root_ = path;
      service_.save_files_selected_root(path.toStdString());
      return;
    }
    file_selected_share_root_.clear();
    service_.save_files_selected_root({});
  }

  const auto scope_roots = service_.share_roots_for_scope(file_scope_group_id_.toStdString());
  if (!scope_roots.empty())
    return;
}

void NodeController::setFilesSection(int section) {
  if (section < 0)
    section = 0;
  if (section > 2)
    section = 2;
  if (files_section_ == section && section != 2)
    return;
  files_section_ = section;
  if (files_section_ == 1 && fileExchangeReady())
    refreshRemoteFileList();
  if (files_section_ == 2) {
    refreshGroupList();
    refreshFileAccessLists();
  }
  emit filesChanged();
  emit fileAccessChanged();
}

std::vector<nyx::FileEntry>
NodeController::remoteRootsCatalog(const std::vector<nyx::FileEntry>& all) const {
  nyx::GroupId scope {};
  if (!file_scope_group_id_.isEmpty()) {
    nyx::GroupStore::group_id_from_hex(file_scope_group_id_.toStdString(), scope);
  }

  std::map<std::string, int> file_counts;
  for (const auto& e : all) {
    if (e.is_directory())
      continue;
    file_counts[nyx::normalize_utf8_path(e.root_path)]++;
  }

  std::map<std::string, nyx::FileEntry> by_root;
  for (const auto& e : all) {
    if (!e.is_directory())
      continue;
    const std::string norm = nyx::normalize_utf8_path(e.root_path);
    nyx::FileEntry marker = e;
    marker.root_path = norm;
    const auto it = file_counts.find(norm);
    if (it != file_counts.end())
      marker.size = static_cast<uint64_t>(it->second);
    by_root[norm] = std::move(marker);
  }

  for (const auto& [path, count] : file_counts) {
    if (by_root.count(path))
      continue;
    nyx::ShareRoot sr;
    sr.path = path;
    sr.group_id = scope;
    by_root[path] = nyx::FileIndex::make_directory_marker(sr, count);
  }

  std::vector<nyx::FileEntry> out;
  out.reserve(by_root.size());
  for (auto& [_, entry] : by_root)
    out.push_back(std::move(entry));
  return out;
}

void NodeController::syncFileBrowseCrumbs() {
  file_browse_crumbs_.clear();
  if (file_selected_share_root_.isEmpty())
    return;

  const QFileInfo rootInfo(file_selected_share_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"),
                   rootInfo.fileName().isEmpty() ? file_selected_share_root_ : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  file_browse_crumbs_.append(rootCrumb);

  QString rel = file_browse_path_;
  if (rel.isEmpty())
    return;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  int from = 0;
  QString built;
  while (from < rel.length()) {
    const int slash = rel.indexOf(QLatin1Char('/'), from);
    const QString segment = slash < 0 ? rel.mid(from) : rel.mid(from, slash - from);
    if (!segment.isEmpty()) {
      built = built.isEmpty() ? segment : built + QLatin1Char('/') + segment;
      QVariantMap crumb;
      crumb.insert(QStringLiteral("label"), segment);
      crumb.insert(QStringLiteral("path"), built);
      file_browse_crumbs_.append(crumb);
    }
    if (slash < 0)
      break;
    from = slash + 1;
  }
}

void NodeController::syncRemoteBrowseCrumbs() {
  file_remote_browse_crumbs_.clear();

  QVariantMap top;
  top.insert(QStringLiteral("label"), QStringLiteral("Ресурсы"));
  top.insert(QStringLiteral("path"), QString());
  top.insert(QStringLiteral("isRoots"), true);
  file_remote_browse_crumbs_.append(top);

  if (file_resources_root_.isEmpty())
    return;

  const QFileInfo rootInfo(file_resources_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"),
                   rootInfo.fileName().isEmpty() ? file_resources_root_ : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  rootCrumb.insert(QStringLiteral("isRoots"), false);
  file_remote_browse_crumbs_.append(rootCrumb);

  QString rel = file_remote_browse_path_;
  if (rel.isEmpty())
    return;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  int from = 0;
  QString built;
  while (from < rel.length()) {
    const int slash = rel.indexOf(QLatin1Char('/'), from);
    const QString segment = slash < 0 ? rel.mid(from) : rel.mid(from, slash - from);
    if (!segment.isEmpty()) {
      built = built.isEmpty() ? segment : built + QLatin1Char('/') + segment;
      QVariantMap crumb;
      crumb.insert(QStringLiteral("label"), segment);
      crumb.insert(QStringLiteral("path"), built);
      crumb.insert(QStringLiteral("isRoots"), false);
      file_remote_browse_crumbs_.append(crumb);
    }
    if (slash < 0)
      break;
    from = slash + 1;
  }
}

void NodeController::setFileSelectedShareRoot(const QString& path) {
  const QString p = path.trimmed();
  if (p.isEmpty())
    return;

  QString canonical;
  const auto scope_roots = service_.share_roots_for_scope(file_scope_group_id_.toStdString());
  for (const auto& r : scope_roots) {
    const QString rp = QString::fromStdString(r.path);
    if (!shareRootPathsEqual(p, rp))
      continue;
    canonical = rp;
    break;
  }
  if (canonical.isEmpty())
    return;
  if (file_selected_share_root_ == canonical)
    return;

  file_selected_share_root_ = canonical;
  resetFileBrowse();
  refreshLocalFileModel();
  service_.save_files_selected_root(canonical.toStdString());
  emit filesChanged();
}

void NodeController::browseIntoFolder(const QString& navPath, const QString& itemRootPath) {
  if (files_section_ == 1) {
    if (file_resources_root_.isEmpty()) {
      QString root = itemRootPath.trimmed();
      if (root.isEmpty())
        root = navPath.trimmed();
      if (root.isEmpty())
        return;
      for (const auto& e : service_.remote_files()) {
        if (shareRootPathsEqual(root, QString::fromStdString(e.root_path))) {
          root = QString::fromStdString(e.root_path);
          break;
        }
      }
      file_resources_root_ = root;
      file_remote_browse_path_.clear();
    } else {
      QString rel = navPath.trimmed();
      rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
      file_remote_browse_path_ = rel;
    }
    syncRemoteBrowseCrumbs();

    service_.request_remote_files_at(file_scope_group_id_.toStdString(),
                                     file_resources_root_.toStdString(),
                                     file_remote_browse_path_.toStdString());
    refreshRemoteFileModel();
    emit filesChanged();
    return;
  }

  if (navPath.trimmed().isEmpty())
    return;
  QString rel = navPath.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  file_browse_path_ = rel;
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  emit filesChanged();
}

void NodeController::browseUp() {
  if (files_section_ == 1) {
    if (!file_remote_browse_path_.isEmpty()) {
      QString rel = file_remote_browse_path_;
      rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
      const int slash = rel.lastIndexOf(QLatin1Char('/'));
      file_remote_browse_path_ = slash < 0 ? QString() : rel.left(slash);
      syncRemoteBrowseCrumbs();
      service_.request_remote_files_at(file_scope_group_id_.toStdString(),
                                       file_resources_root_.toStdString(),
                                       file_remote_browse_path_.toStdString());
      refreshRemoteFileModel();
    } else if (!file_resources_root_.isEmpty()) {
      file_resources_root_.clear();
      syncRemoteBrowseCrumbs();
      if (fileExchangeReady()) {
        service_.request_remote_files_at(file_scope_group_id_.toStdString(), {}, {});
      }
      refreshRemoteFileModel();
    }
    emit filesChanged();
    return;
  }

  if (!file_browse_path_.isEmpty()) {
    QString rel = file_browse_path_;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const int slash = rel.lastIndexOf(QLatin1Char('/'));
    file_browse_path_ = slash < 0 ? QString() : rel.left(slash);
    syncFileBrowseCrumbs();
    refreshLocalFileModel();
    emit filesChanged();
    return;
  }

  if (!file_selected_share_root_.isEmpty()) {
    file_selected_share_root_.clear();
    resetFileBrowse();
    service_.save_files_selected_root({});
    refreshLocalFileModel();
    emit filesChanged();
  }
}

void NodeController::browseToCrumb(int index) {
  if (files_section_ == 1) {
    if (index < 0 || index >= file_remote_browse_crumbs_.size())
      return;
    const QVariantMap crumb = file_remote_browse_crumbs_.at(index).toMap();
    if (crumb.value(QStringLiteral("isRoots")).toBool() || index == 0) {
      file_resources_root_.clear();
      file_remote_browse_path_.clear();
      syncRemoteBrowseCrumbs();
      if (fileExchangeReady()) {
        service_.request_remote_files_at(file_scope_group_id_.toStdString(), {}, {});
      }
      refreshRemoteFileModel();
      emit filesChanged();
      return;
    }
    if (index == 1) {
      file_remote_browse_path_.clear();
    } else {
      file_remote_browse_path_ = crumb.value(QStringLiteral("path")).toString();
    }
    syncRemoteBrowseCrumbs();
    if (!file_resources_root_.isEmpty() && fileExchangeReady()) {
      service_.request_remote_files_at(file_scope_group_id_.toStdString(),
                                       file_resources_root_.toStdString(),
                                       file_remote_browse_path_.toStdString());
    }
    refreshRemoteFileModel();
    emit filesChanged();
    return;
  }

  if (index < 0 || index >= file_browse_crumbs_.size())
    return;
  file_browse_path_ =
      file_browse_crumbs_.at(index).toMap().value(QStringLiteral("path")).toString();
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  emit filesChanged();
}

void NodeController::addDroppedUrls(const QVariantList& urls) {
  if (urls.isEmpty())
    return;
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  if (file_index_busy_.load()) {
    showToast(QStringLiteral("Индексация уже выполняется"));
    return;
  }
  QStringList dirs;
  for (const QVariant& u : urls) {
    QString p = u.toString().trimmed();
    if (p.startsWith(QStringLiteral("file:///")))
      p = QUrl(p).toLocalFile();
    if (p.isEmpty())
      continue;
    QFileInfo info(p);
    if (info.isDir())
      dirs.push_back(info.absoluteFilePath());
    else if (info.isFile())
      dirs.push_back(info.absolutePath());
  }
  if (dirs.isEmpty()) {
    showToast(QStringLiteral("Не удалось добавить из перетаскивания"));
    return;
  }
  runIndexJob(dirs.front(), file_scope_group_id_, false);
  for (int i = 1; i < dirs.size(); ++i) {

    Q_UNUSED(i);
  }
  if (dirs.size() > 1) {
    showToast(QStringLiteral("Индексируется первая папка; остальные добавьте по очереди"));
  }
}

void NodeController::syncFileScopeLabel() {
  if (file_scope_group_id_.isEmpty()) {
    file_scope_label_ = QStringLiteral("Личные файлы");
    return;
  }
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == file_scope_group_id_) {
      file_scope_label_ = m.value(QStringLiteral("name")).toString();
      return;
    }
  }
  file_scope_label_ = file_scope_group_id_.left(8) + QStringLiteral("…");
}

void NodeController::refreshFileShareRoots() {
  file_share_roots_.clear();
  const auto roots = service_.share_roots_for_scope(file_scope_group_id_.toStdString());
  for (const auto& r : roots) {
    const QString path = QString::fromStdString(r.path);
    const QString scopeId =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    QVariantMap m;
    m.insert(QStringLiteral("path"), path);
    const QFileInfo fi(path);
    m.insert(QStringLiteral("displayName"), fi.fileName().isEmpty() ? path : fi.fileName());
    m.insert(QStringLiteral("isPersonal"), r.is_personal());
    m.insert(QStringLiteral("scopeGroupId"), scopeId);
    m.insert(QStringLiteral("scopeLabel"), scopeLabelForGroupId(scopeId));
    m.insert(QStringLiteral("fileCount"),
             service_.file_count_in_root(r.path, file_scope_group_id_.toStdString()));
    m.insert(QStringLiteral("canRemove"), canRemoveShareRoot(r));
    file_share_roots_.append(m);
  }

  if (!file_selected_share_root_.isEmpty()) {
    bool found = false;
    for (const QVariant& v : file_share_roots_) {
      const QString rp = v.toMap().value(QStringLiteral("path")).toString();
      if (shareRootPathsEqual(rp, file_selected_share_root_)) {
        file_selected_share_root_ = rp;
        found = true;
        break;
      }
    }
    if (!found)
      file_selected_share_root_.clear();
  }
  if (file_selected_share_root_.isEmpty() && !file_share_roots_.isEmpty()) {
    file_selected_share_root_ =
        file_share_roots_.first().toMap().value(QStringLiteral("path")).toString();
    resetFileBrowse();
  }
}

void NodeController::refreshLocalFileModel() {
  if (file_selected_share_root_.isEmpty()) {

    const auto all = service_.local_files_for_scope(file_scope_group_id_.toStdString());
    const std::string objects_prefix = nyx::normalize_utf8_path(nyx::data_dir() + "/objects") + "/";
    const std::string library_prefix =
        nyx::normalize_utf8_path(nyx::FileIndex::library_root_path([&] {
          nyx::GroupId scope {};
          if (!file_scope_group_id_.isEmpty()) {
            nyx::GroupStore::group_id_from_hex(file_scope_group_id_.toStdString(), scope);
          }
          return scope;
        }()));
    std::vector<nyx::FileEntry> managed;
    managed.reserve(all.size());
    for (const auto& e : all) {
      if (e.is_directory())
        continue;
      const std::string root = nyx::normalize_utf8_path(e.root_path);
      if (root.rfind(objects_prefix, 0) == 0 || root == library_prefix ||
          root.rfind(library_prefix + "/", 0) == 0) {
        managed.push_back(e);
      }
    }
    local_file_list_ = entriesToVariant(managed, false);
    return;
  }
  std::string root_path = file_selected_share_root_.toStdString();
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(file_selected_share_root_, QString::fromStdString(r.path))) {
      root_path = r.path;
      break;
    }
  }
  const auto entries = service_.local_files_at_root(
      root_path, file_browse_path_.toStdString(), file_scope_group_id_.toStdString());
  local_file_list_ = entriesToVariant(entries, false);
}

void NodeController::reconcileRemoteBrowsePath(const std::vector<nyx::FileEntry>& catalog) {
  if (file_resources_root_.isEmpty())
    return;
  bool found = false;
  for (const auto& e : catalog) {
    if (e.root_path.empty())
      continue;
    if (shareRootPathsEqual(file_resources_root_, QString::fromStdString(e.root_path))) {
      found = true;
      break;
    }
  }
  if (!found) {
    for (const auto& e : remoteRootsCatalog(catalog)) {
      if (shareRootPathsEqual(file_resources_root_, QString::fromStdString(e.root_path))) {
        found = true;
        break;
      }
    }
  }
  if (!found) {
    file_resources_root_.clear();
    file_remote_browse_path_.clear();
  }
}

void NodeController::refreshRemoteFileModel() {
  refreshRemoteFileModel(service_.remote_files());
}

void NodeController::refreshRemoteFileModel(const std::vector<nyx::FileEntry>& entries) {
  reconcileRemoteBrowsePath(entries);
  std::vector<nyx::FileEntry> level;
  if (file_resources_root_.isEmpty()) {
    level = remoteRootsCatalog(entries);
  } else {
    std::string root_path = file_resources_root_.toStdString();
    for (const auto& e : entries) {
      if (shareRootPathsEqual(file_resources_root_, QString::fromStdString(e.root_path))) {
        root_path = e.root_path;
        break;
      }
    }
    level =
        nyx::FileIndex::listing_level(entries, root_path, file_remote_browse_path_.toStdString());
  }
  remote_file_list_ = entriesToVariant(level, true);
  syncRemoteBrowseCrumbs();
}

void NodeController::refreshFileLists() {
  refreshFileShareRoots();
  refreshLocalFileModel();
  refreshRemoteFileModel();
  emit filesChanged();
}

QString NodeController::pickFolder() {
#if defined(Q_OS_ANDROID)

  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/shares");
  QDir().mkpath(base);
  bool ok = false;
  const QString name = QInputDialog::getText(
      nullptr,
      QStringLiteral("Папка для обмена"),
      QStringLiteral("Создайте папку в хранилище Nyx и выберите файлы "
                     "(Documents, Downloads и т.п.) для копирования в неё.\n\n"
                     "Имя папки:"),
      QLineEdit::Normal,
      QStringLiteral("Documents"),
      &ok);
  if (!ok)
    return {};
  QString clean = name.trimmed();
  clean.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
  if (clean.isEmpty())
    clean = QStringLiteral("shared");
  const QString path = QDir(base).filePath(clean);
  if (!QDir().mkpath(path)) {
    showToast(QStringLiteral("Не удалось создать папку"), true);
    return {};
  }

  const QList<QUrl> urls =
      QFileDialog::getOpenFileUrls(nullptr,
                                   QStringLiteral("Файлы в папку «%1»").arg(clean),
                                   QUrl(),
                                   QStringLiteral("Все файлы (*.*)"));
  int copied = 0;
  for (const QUrl& url : urls) {
    QString source;
    QString file_name;
    bool temporary = false;
    if (url.scheme() == QLatin1String("content")) {
      const auto info = nyx_android::storage_document_info(url.toString());
      file_name = QFileInfo(info.name).fileName();
      if (file_name.isEmpty()) {
        file_name = QStringLiteral("file-%1").arg(++copied);
      }
      source = QDir(path).filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                   QLatin1Char('-') + file_name);
      if (!nyx_android::copy_content_uri(url.toString(), source))
        continue;

      const QString final_path = QDir(path).filePath(file_name);
      if (final_path != source) {
        QFile::remove(final_path);
        QFile::rename(source, final_path);
      }
      ++copied;
      continue;
    }
    source = url.toLocalFile();
    file_name = QFileInfo(source).fileName();
    if (file_name.isEmpty() || source.isEmpty())
      continue;
    const QString dest = QDir(path).filePath(file_name);
    QFile::remove(dest);
    if (QFile::copy(source, dest))
      ++copied;
  }
  showToast(copied > 0 ? QStringLiteral("В папку скопировано файлов: %1").arg(copied)
                       : QStringLiteral("Папка создана — можно добавить файлы позже"));
  return path;
#else
  const QString dir = QFileDialog::getExistingDirectory(
      nullptr, QStringLiteral("Выберите папку для индекса"), QDir::homePath());
  return dir;
#endif
}

QString NodeController::pickSaveFile(const QString& suggestedFileName) {
  const QString name = suggestedFileName.trimmed().isEmpty() ? QStringLiteral("download")
                                                             : suggestedFileName.trimmed();
#if defined(Q_OS_ANDROID)
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/downloads");
  QDir().mkpath(downloads);
  return QDir(downloads).filePath(name);
#else
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString base = downloads.isEmpty() ? QDir::homePath() : downloads;
  const QString suggested = QDir(base).filePath(name);
  return QFileDialog::getSaveFileName(
      nullptr, QStringLiteral("Сохранить файл"), suggested, QStringLiteral("Все файлы (*.*)"));
#endif
}

QString NodeController::pickSaveFolder() {
#if defined(Q_OS_ANDROID)
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                            QStringLiteral("/downloads");
  QDir().mkpath(downloads);
  return downloads;
#else
  const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString start = downloads.isEmpty() ? QDir::homePath() : downloads;
  return QFileDialog::getExistingDirectory(
      nullptr, QStringLiteral("Выберите папку для сохранения"), start);
#endif
}

void NodeController::runIndexJob(const QString& path, const QString& scopeGroupId, bool rescan) {
  const QString p = path.trimmed();
  if (p.isEmpty())
    return;
  bool expected = false;
  if (!file_index_busy_.compare_exchange_strong(expected, true)) {
    showToast(QStringLiteral("Индексация уже выполняется"));
    return;
  }

  file_index_progress_visible_ = true;
  file_index_progress_percent_ = 5;
  file_index_progress_label_ = QStringLiteral("Подготовка сканирования…");
  file_index_files_scanned_ = 0;
  emit fileIndexProgressChanged();

  const QString scope = scopeGroupId;
  if (file_index_thread_.joinable())
    file_index_thread_.join();
  file_index_thread_ = std::thread([this, p, scope, rescan]() {
    const bool ok = rescan ? service_.rescan_share_root(p.toStdString(), scope.toStdString())
                           : service_.index_folder(p.toStdString(), scope.toStdString());
    const int count = ok ? service_.file_count_in_root(p.toStdString(), scope.toStdString()) : 0;
    QMetaObject::invokeMethod(
        this,
        [this, ok, count, p, rescan]() {
          file_index_busy_.store(false);
          file_index_progress_visible_ = true;
          file_index_progress_percent_ = 100;
          if (!ok) {
            file_index_progress_label_ = QStringLiteral("Ошибка индексации");
            showToast(status_text_.isEmpty()
                          ? (rescan ? QStringLiteral("Не удалось переиндексировать")
                                    : QStringLiteral("Не удалось проиндексировать папку"))
                          : status_text_);
          } else {
            file_index_progress_label_ = QStringLiteral("Готово: %1 файлов").arg(count);
            refreshFileLists();
            if (!rescan) {
              setFileSelectedShareRoot(p);
              if (count == 0) {
                showToast(QStringLiteral(
                    "Папка добавлена (0 файлов). Положите файлы и нажмите «Переиндексировать»."));
              } else {
                showToast(QStringLiteral("Папка проиндексирована: %1 файлов").arg(count));
              }
            } else {
              showToast(QStringLiteral("Переиндексировано: %1 файлов").arg(count));
            }
          }
          emit fileIndexProgressChanged();
          QTimer::singleShot(1400, this, [this]() {
            if (!file_index_busy_.load()) {
              file_index_progress_visible_ = false;
              emit fileIndexProgressChanged();
            }
          });
        },
        Qt::QueuedConnection);
  });
}

void NodeController::addIndexedFolder(const QString& path) {
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  QString p = path.trimmed();
  if (p.isEmpty()) {
    p = pickFolder();
    if (p.isEmpty())
      return;
  }
  if (p.startsWith(QStringLiteral("file:///")))
    p = QUrl(p).toLocalFile();
  runIndexJob(p, file_scope_group_id_, false);
}

void NodeController::removeIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty())
    return;

  QString scope = file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path)))
      continue;
    const QString root_scope =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    if (root_scope != scope)
      continue;
    if (!canRemoveShareRoot(r)) {
      showToast(QStringLiteral("Нет права убирать эту папку"));
      return;
    }
    break;
  }
  if (!service_.remove_share_root(path.toStdString(), scope.toStdString())) {
    showToast(status_text_.isEmpty() ? QStringLiteral("Не удалось убрать папку") : status_text_);
    return;
  }
  if (shareRootPathsEqual(file_selected_share_root_, path)) {
    file_selected_share_root_.clear();
    resetFileBrowse();
    service_.save_files_selected_root({});
  }
  if (shareRootPathsEqual(file_resources_root_, path)) {
    file_resources_root_.clear();
    file_remote_browse_path_.clear();
  }
  refreshFileLists();
  if (files_section_ == 1 && fileExchangeReady())
    refreshRemoteFileList();
  showToast(QStringLiteral("Папка убрана из индекса"));
}

void NodeController::rescanIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty())
    return;
  QString scope = file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path)))
      continue;
    const QString root_scope =
        r.is_personal()
            ? QString()
            : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    if (root_scope != scope)
      continue;
    break;
  }
  runIndexJob(path.trimmed(), scope, true);
}

void NodeController::refreshRemoteFileList() {

  file_resources_root_.clear();
  file_remote_browse_path_.clear();
  syncRemoteBrowseCrumbs();
  if (!service_.request_remote_files_at(file_scope_group_id_.toStdString(), {}, {})) {
    refreshRemoteFileModel();
    emit filesChanged();
    showToast(fileExchangeHint().isEmpty() ? QStringLiteral("Не удалось запросить файлы")
                                           : fileExchangeHint());
    return;
  }
  emit filesChanged();
}

void NodeController::downloadFile(const QString& hashHex,
                                  const QString& fileName,
                                  const QString& rootPath,
                                  const QString& relativePath) {
  if (!fileExchangeReady()) {
    showToast(fileExchangeHint().isEmpty()
                  ? QStringLiteral("Подключитесь к полю или чату для скачивания")
                  : fileExchangeHint());
    return;
  }
  if (!canFileDownloadAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права скачивать файлы"));
    return;
  }
  const QString hash = hashHex.trimmed();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Неверный hash файла"));
    return;
  }
  const QString suggested =
      fileName.trimmed().isEmpty() ? QStringLiteral("download") : fileName.trimmed();
  const QString dest = pickSaveFile(suggested);
  if (dest.isEmpty())
    return;
  if (!service_.download_file(hash.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Не удалось скачать файл"));
    return;
  }
  showToast(QStringLiteral("Скачивание…"));
}

void NodeController::downloadRemoteFolder(const QString& rootPath, const QString& relativePath) {
  if (!canFileDownloadAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права скачивать файлы"));
    return;
  }
  const QString destDir = pickSaveFolder();
  if (destDir.isEmpty())
    return;
  const QString canonical = resolveAccessRootPath(rootPath);
  std::string root = canonical.toStdString();
  for (const auto& e : service_.remote_files()) {
    if (!shareRootPathsEqual(canonical, QString::fromStdString(e.root_path)))
      continue;
    root = e.root_path;
    break;
  }
  const std::size_t queued =
      service_.enqueue_folder_downloads(root, relativePath.toStdString(), destDir.toStdString());
  if (queued == 0) {
    showToast(QStringLiteral("В папке нет файлов для скачивания"));
    return;
  }
  showToast(QStringLiteral("Скачивание %1 файлов…").arg(static_cast<qulonglong>(queued)));
}

void NodeController::sendFileByHash(const QString& hashHex) {
  if (!canFileUpload()) {
    showToast(QStringLiteral("Нет права отправлять файлы"));
    return;
  }
  if (!service_.send_file(hashHex.trimmed().toStdString())) {
    showToast(QStringLiteral("Не удалось отправить файл"));
  }
}

uint32_t NodeController::filePermissionsAt(const QString& rootPath,
                                           const QString& relativePath) const {
  if (file_scope_group_id_.isEmpty())
    return nyx::kFilePermissionAll;
  if (isFileScopeOwner())
    return nyx::kFilePermissionAll;
  const QString root = resolveAccessRootPath(rootPath);
  return service_.my_file_permissions(
      file_scope_group_id_.toStdString(), root.toStdString(), relativePath.toStdString());
}

uint32_t NodeController::currentFilePermissions() const {
  if (file_scope_group_id_.isEmpty())
    return nyx::kFilePermissionAll;
  if (isFileScopeOwner())
    return nyx::kFilePermissionAll;

  QString root;
  QString rel;
  if (files_section_ == 1) {
    root = file_resources_root_;
    rel = file_remote_browse_path_;
  } else if (files_section_ == 0) {
    root = file_selected_share_root_;
    rel = file_browse_path_;
  }
  return filePermissionsAt(root, rel);
}

QString NodeController::fileLocalPath(const QString& hashHex) const {
  return mediaLocalPath(hashHex);
}

void NodeController::ensureFileAvailable(const QString& hashHex, const QString& fileName) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64 || !fileLocalPath(hex).isEmpty())
    return;
  QString safe_name = QFileInfo(fileName).fileName();
  if (safe_name.isEmpty())
    safe_name = hex;
  const QString dir = QString::fromStdString(nyx::default_downloads_dir());
  QDir().mkpath(dir);
  const QString dest = QDir(dir).filePath(hex.left(12) + QLatin1Char('-') + safe_name);
  if (!service_.download_file(hex.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Нет доступного источника файла"), true);
  }
}

QString NodeController::fileTextPreview(const QString& hashHex) const {
  const QString path = fileLocalPath(hashHex);
  if (path.isEmpty() || path.endsWith(QLatin1String(".part")))
    return {};

  if (!service_.find_file_object(hashHex.trimmed().toLower().toStdString()) &&
      !path.contains(QStringLiteral("/objects/")) && !path.contains(QStringLiteral("/library/")) &&
      !path.contains(QStringLiteral("/chat_media/")) &&
      !path.contains(QStringLiteral("/downloads/"))) {
    return {};
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  const QByteArray data = file.read(256 * 1024 + 1);
  if (data.size() > 256 * 1024 || data.contains('\0'))
    return {};
  return QString::fromUtf8(data);
}

bool NodeController::openLocalFile(const QString& path, const QString& mime) {
  const QString local = path.trimmed();
  if (local.isEmpty() || local.endsWith(QLatin1String(".part"))) {
    showToast(QStringLiteral("Файл ещё не готов"), true);
    return false;
  }
  if (!QFileInfo::exists(local)) {
    showToast(QStringLiteral("Файл не найден"), true);
    return false;
  }
  QString use_mime = mime.trimmed();
  if (use_mime.isEmpty()) {

    use_mime = QMimeDatabase().mimeTypeForFile(local, QMimeDatabase::MatchExtension).name();
  }
  if (use_mime.startsWith(QLatin1String("image/")) ||
      use_mime.startsWith(QLatin1String("audio/")) ||
      use_mime.startsWith(QLatin1String("video/"))) {
    openInAppMedia(local, use_mime, QFileInfo(local).fileName());
    return true;
  }
  if (DocumentViewer::canHandle(local, use_mime)) {
    closeInAppMedia();
    return document_viewer_.openDocument(local, use_mime, QFileInfo(local).fileName());
  }
#if defined(Q_OS_ANDROID)
  if (nyx_android::open_file(local, use_mime))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#elif defined(Q_OS_LINUX)
  const QString abs = QFileInfo(local).absoluteFilePath();
  const QProcessEnvironment env = nyx_app::host_process_environment();

  auto launch = [&](const QString& program, const QStringList& args) -> bool {
    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.setProgram(program);
    proc.setArguments(args);
    proc.setWorkingDirectory(QFileInfo(abs).absolutePath());
    return proc.startDetached();
  };
  if (launch(QStringLiteral("/usr/bin/xdg-open"), {abs}))
    return true;
  if (launch(QStringLiteral("xdg-open"), {abs}))
    return true;
  if (launch(QStringLiteral("/usr/bin/gio"), {QStringLiteral("open"), abs}))
    return true;

  if (QDesktopServices::openUrl(QUrl::fromLocalFile(abs)))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#else
  if (QDesktopServices::openUrl(QUrl::fromLocalFile(local)))
    return true;
  showToast(QStringLiteral("Не удалось открыть файл"), true);
  return false;
#endif
}

void NodeController::openFileByHash(const QString& hashHex,
                                    const QString& fileName,
                                    const QString& mime,
                                    const QString& rootPath,
                                    const QString& relativePath) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64) {
    showToast(QStringLiteral("Некорректный hash файла"), true);
    return;
  }
  QString path = fileLocalPath(hex);

  if (path.isEmpty() && !rootPath.trimmed().isEmpty() && !relativePath.trimmed().isEmpty()) {
    const QString candidate = QDir(rootPath.trimmed()).filePath(relativePath.trimmed());
    if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
      path = candidate;
    }
  }
  if (path.isEmpty()) {
    ensureFileAvailable(hex, fileName);
    showToast(QStringLiteral("Файл загружается — откроется, когда будет готов"));

    QTimer::singleShot(1200, this, [this, hex, fileName, mime, rootPath, relativePath]() {
      QString ready = fileLocalPath(hex);
      if (ready.isEmpty() && !rootPath.trimmed().isEmpty() && !relativePath.trimmed().isEmpty()) {
        const QString candidate = QDir(rootPath.trimmed()).filePath(relativePath.trimmed());
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile())
          ready = candidate;
      }
      if (ready.isEmpty())
        return;
      QString use_mime = mime;
      if (use_mime.isEmpty()) {
        use_mime = QMimeDatabase().mimeTypeForFile(ready, QMimeDatabase::MatchExtension).name();
      }
      openLocalFile(ready, use_mime);
    });
    return;
  }
  QString use_mime = mime;
  if (use_mime.isEmpty()) {
    use_mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name();
  }
  openLocalFile(path, use_mime);
}

int NodeController::fileSyncState(const QString& hashHex) const {

  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return 0;
  if (!fileLocalPath(hex).isEmpty())
    return 2;
  for (const auto& item : transfer_queue_) {
    const QVariantMap m = item.toMap();
    if (m.value(QStringLiteral("hash")).toString() != hex)
      continue;
    const QString state = m.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("active") || state == QLatin1String("queued")) {
      return 1;
    }
  }
  return 0;
}

void NodeController::linkFileToChat(const QString& hashHex,
                                    const QString& fileName,
                                    const QString& mime,
                                    qulonglong size) {
  const QString hash = hashHex.trimmed().toLower();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Некорректный hash файла"), true);
    return;
  }
  QString name = fileName;
  name.replace(QLatin1Char(']'), QLatin1Char('_'));
  QString safe_mime = mime.trimmed();
  safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
  if (safe_mime.isEmpty()) {
    safe_mime = QStringLiteral("application/octet-stream");
  }
  const QString markdown = QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
                               .arg(name, hash, QString::number(size), safe_mime);
  showChatView();
  QTimer::singleShot(0, this, [this, markdown]() { emit fileLinkReady(markdown); });
}

void NodeController::linkFolderToChat(const QString& hashHex,
                                      const QString& folderName,
                                      const QString& rootPath,
                                      const QString& relativePath,
                                      qulonglong size) {
  const QString hash = hashHex.trimmed().toLower();
  if (hash.size() != 64) {
    showToast(QStringLiteral("Некорректный hash папки"), true);
    return;
  }
  QString name = folderName.trimmed();
  name.replace(QLatin1Char(']'), QLatin1Char('_'));
  if (name.isEmpty())
    name = QStringLiteral("Папка");
  QString root = rootPath.trimmed();
  root.replace(QLatin1Char(')'), QLatin1Char('_'));
  QString rel = relativePath.trimmed();
  rel.replace(QLatin1Char(')'), QLatin1Char('_'));
  const QString markdown =
      QStringLiteral("[%1](nyx-file:%2;size=%3;mime=application/x-nyx-directory;root=%4;rel=%5)")
          .arg(name,
               hash,
               QString::number(size),
               QString::fromUtf8(root.toUtf8().toPercentEncoding()),
               QString::fromUtf8(rel.toUtf8().toPercentEncoding()));
  showChatView();
  QTimer::singleShot(0, this, [this, markdown]() { emit fileLinkReady(markdown); });
}

void NodeController::openFolderInResources(const QString& hashHex,
                                           const QString& rootPath,
                                           const QString& relativePath) {
  setMainViewMode(1);
  setFilesSection(1);
  QString root = QUrl::fromPercentEncoding(rootPath.toUtf8()).trimmed();
  QString rel = QUrl::fromPercentEncoding(relativePath.toUtf8()).trimmed();
  if (root.isEmpty()) {
    const QString hex = hashHex.trimmed().toLower();
    for (const auto& e : service_.remote_files()) {
      if (nyx::hash_hex(e.hash) != hex.toStdString())
        continue;
      root = QString::fromStdString(e.root_path);
      rel = QString::fromStdString(e.relative_path);
      break;
    }
    if (root.isEmpty()) {
      for (const auto& e : service_.local_files_for_scope(file_scope_group_id_.toStdString())) {
        if (nyx::hash_hex(e.hash) != hex.toStdString())
          continue;
        root = QString::fromStdString(e.root_path);
        rel = QString::fromStdString(e.relative_path);
        break;
      }
    }
  }
  if (root.isEmpty()) {
    showToast(QStringLiteral("Папка не найдена в ресурсах"), true);
    return;
  }
  file_resources_root_ = root;

  file_remote_browse_path_ = rel;
  if (fileExchangeReady()) {
    service_.request_remote_files_at(
        file_scope_group_id_.toStdString(), root.toStdString(), rel.toStdString());
  }
  refreshRemoteFileModel();
  emit filesChanged();
}

void NodeController::pauseFileTransfer(const QString& hashHex, bool paused) {
  service_.pause_transfer(hashHex.toStdString(), paused);
}

void NodeController::cancelFileTransfer(const QString& hashHex) {
  service_.cancel_transfer(hashHex.toStdString());
}

void NodeController::retryFileTransfer(const QString& hashHex) {
  service_.retry_transfer(hashHex.toStdString());
}

void NodeController::moveFileTransfer(const QString& hashHex, int delta) {
  service_.move_transfer(hashHex.toStdString(), delta);
}

void NodeController::importFiles() {
  const QList<QUrl> urls = QFileDialog::getOpenFileUrls(
      nullptr, QStringLiteral("Импортировать файлы"), QUrl(), QStringLiteral("Все файлы (*.*)"));
  if (urls.isEmpty())
    return;
  int imported = 0;
  for (const QUrl& url : urls) {
    QString source;
    QString name;
    QString mime = QStringLiteral("application/octet-stream");
    bool temporary = false;
#if defined(Q_OS_ANDROID)
    if (url.scheme() == QLatin1String("content")) {
      const auto info = nyx_android::storage_document_info(url.toString());
      name = QFileInfo(info.name).fileName();
      if (name.isEmpty())
        name = QStringLiteral("imported-file");
      mime = info.mime;
      const QString staging = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/import-staging");
      QDir().mkpath(staging);
      source = QDir(staging).filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                      QLatin1Char('-') + name);
      if (!nyx_android::copy_content_uri(url.toString(), source))
        continue;
      temporary = true;
    } else
#endif
    {
      source = url.toLocalFile();
      name = QFileInfo(source).fileName();
      mime = QMimeDatabase().mimeTypeForFile(source).name();
    }
    const auto object = service_.import_file_object(source.toStdString(),
                                                    name.toStdString(),
                                                    mime.toStdString(),
                                                    file_scope_group_id_.toStdString());
    if (temporary)
      QFile::remove(source);
    if (object)
      ++imported;
  }
  refreshFileLists();
  showToast(QStringLiteral("Импортировано файлов: %1").arg(imported), imported == 0);
}

void NodeController::exportFile(const QString& hashHex,
                                const QString& fileName,
                                const QString& mime) {
  Q_UNUSED(mime);
  const QString source = fileLocalPath(hashHex);
  if (source.isEmpty()) {
    showToast(QStringLiteral("Файл ещё не загружен"), true);
    return;
  }
#if defined(Q_OS_ANDROID)
  if (!nyx_android::export_file(source, fileName, mime)) {
    showToast(QStringLiteral("Не удалось экспортировать файл"), true);
  }
#else
  const QString destination =
      QFileDialog::getSaveFileName(nullptr, QStringLiteral("Экспортировать файл"), fileName);
  if (destination.isEmpty())
    return;
  QFile::remove(destination);
  if (!QFile::copy(source, destination)) {
    showToast(QStringLiteral("Не удалось экспортировать файл"), true);
  }
#endif
}
