#include "node_controller.hpp"

#include "nyx/account_store.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/profile_crypto.hpp"
#include "nyx/conversation.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_index.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVariantMap>

namespace {

QString normalizeSessionKey(const QString& key) {
  if (key.startsWith(QLatin1String("group:")) || key.startsWith(QLatin1String("dm:"))) {
    return key.section(QLatin1Char(':'), 0, 0) + QLatin1Char(':') +
           key.section(QLatin1Char(':'), 1).toLower();
  }
  return key;
}

}  // namespace

#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace {

QString formatFileSizeLabel(quint64 bytes, bool is_directory) {
  if (is_directory) {
    if (bytes == 0) return QStringLiteral("пустая папка");
    if (bytes == 1) return QStringLiteral("1 файл");
    return QStringLiteral("%1 файлов").arg(bytes);
  }
  if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
  if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
  if (bytes < 1024ULL * 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
  }
  return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + QStringLiteral(" GB");
}

QString utf8q(const std::string& s) {
  if (s.empty()) return {};
  const auto n = static_cast<qsizetype>(
      std::min(s.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return QString::fromUtf8(s.data(), n);
}

QIcon makeTrayIcon() {
  QIcon icon(QStringLiteral(":/icons/nyx-mark.svg"));
  if (!icon.isNull()) return icon;
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter painter(&pm);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setBrush(QColor("#5288c1"));
  painter.setPen(Qt::NoPen);
  painter.drawEllipse(2, 2, 28, 28);
  return QIcon(pm);
}

bool parse_user_id_hex(const QString& hex, nyx::UserId& out) {
  std::vector<uint8_t> bytes;
  if (!nyx::from_hex(hex.toStdString(), bytes) || bytes.size() != out.size()) return false;
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return true;
}

}  // namespace

NodeController::NodeController(QObject* parent) : QObject(parent) {
  if (QSystemTrayIcon::isSystemTrayAvailable()) {
    tray_icon_ = new QSystemTrayIcon(makeTrayIcon(), this);
    tray_icon_->setToolTip(QStringLiteral("Nyx"));
    tray_menu_ = new QMenu(nullptr);
    auto* show_action = tray_menu_->addAction(QStringLiteral("Открыть Nyx"));
    connect(show_action, &QAction::triggered, this, &NodeController::showWindow);
    tray_menu_->addSeparator();
    auto* quit_action = tray_menu_->addAction(QStringLiteral("Выход"));
    connect(quit_action, &QAction::triggered, qApp, &QCoreApplication::quit);
    tray_icon_->setContextMenu(tray_menu_);
    connect(tray_icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
      if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) showWindow();
    });
    tray_icon_->show();
  }

  wireCallbacks();

  refreshAccountList();
  account_gate_error_.clear();
  legacy_profile_pending_ = nyx::legacy_profile_pending();
  last_account_id_ = QString::fromStdString(nyx::last_account_id());
  emit accountGateChanged();

  if (!last_account_id_.isEmpty()) {
    tryUnlockRemembered(last_account_id_);
  }
}

void NodeController::beginMainSession() {
  resetFilesUiState();
  service_.reload_account_data();

  const std::string saved_scope = service_.load_files_scope_group_id();
  if (!saved_scope.empty()) {
    file_scope_group_id_ = QString::fromStdString(saved_scope).trimmed().toLower();
  }
  const std::string saved_root = service_.load_files_selected_root();
  if (!saved_root.empty()) {
    file_selected_share_root_ = QString::fromStdString(saved_root);
  }
  syncFileScopeFromSavedOrRoots();
  syncFileScopeLabel();
  resetFileBrowse();

  refreshFileLists();

  service_.load_network_config();
  syncNetworkSettingsFromService();
  refreshProfile();
  refreshChatList();
  refreshGroupList();
  refreshFileAccessLists();

  lan_discovery_timer_.setInterval(5000);
  connect(&lan_discovery_timer_, &QTimer::timeout, this, &NodeController::tickLanDiscovery);
  lan_discovery_timer_.start();
  QTimer::singleShot(800, this, &NodeController::tickLanDiscovery);
  QTimer::singleShot(400, this, [this]() {
    service_.ensure_owned_hubs_running();
    refreshChatList();
    emit sessionsChanged();
  });
  QTimer::singleShot(1500, this, &NodeController::maybeAutoReconnectSessions);

  session_reconnect_timer_.setInterval(5000);
  connect(&session_reconnect_timer_, &QTimer::timeout, this,
          &NodeController::maybeAutoReconnectSessions);
  session_reconnect_timer_.start();
}

NodeController::~NodeController() {
  lan_discovery_timer_.stop();
  session_reconnect_timer_.stop();
  if (tray_icon_) {
    tray_icon_->hide();
  }
  service_.stop();
  nyx::lock_session();
}

void NodeController::syncNetworkSettingsFromService() {
  rendezvous_list_ = QString::fromStdString(service_.rendezvous_list_string());
  if (rendezvous_list_.isEmpty()) {
    rendezvous_list_ = rendezvous_;
  } else {
    const auto primary = service_.network_config().primary_rendezvous();
    rendezvous_ = QString::fromStdString(primary.host) + QLatin1Char(':') +
                  QString::number(primary.port);
  }
  discovery_mode_ = static_cast<int>(service_.network_config().mode);
  auto_start_owned_hub_ = service_.auto_start_owned_hub();
  emit rendezvousChanged();
  emit networkSettingsChanged();
}

void NodeController::setAutoStartOwnedHub(bool enabled) {
  if (auto_start_owned_hub_ == enabled) return;
  auto_start_owned_hub_ = enabled;
  service_.set_auto_start_owned_hub(enabled);
  emit networkSettingsChanged();
}

bool NodeController::activeFieldIsOwner() const {
  if (active_chat_kind_ != static_cast<int>(nyx::ConversationKind::Group)) return false;
  if (active_chat_ref_id_.isEmpty()) return false;

  nyx::Profile profile;
  if (!nyx::active_profile(profile)) return false;

  nyx::GroupStore store;
  store.load();
  nyx::GroupId group_id{};
  if (!nyx::GroupStore::group_id_from_hex(active_chat_ref_id_.toStdString(), group_id)) {
    return false;
  }
  const auto group = store.find(group_id);
  if (!group) return false;
  return group->owner_id == profile.user_id();
}

void NodeController::maybeAutoReconnectSessions() {
  // Свои hub — если intent включён (после входа ensure_owned_hubs_running его включает).
  // Чужие join/DM — по master-switch внутри auto_reconnect_all.
  service_.auto_reconnect_all();
  invite_token_ = QString::fromStdString(service_.dm_inbox_token_hex());
  emit inviteTokenChanged();
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
  refreshGroupList();
  refreshChatList();
}

QString NodeController::sessionSummary() const {
  const std::size_t n = service_.live_session_count();
  if (n == 0) return QStringLiteral("Нет активных сессий");
  if (n == 1) return QStringLiteral("1 активная сессия");
  return QStringLiteral("%1 активных сессий").arg(static_cast<int>(n));
}

bool NodeController::canSendMessage() const {
  if (active_chat_key_.isEmpty()) return false;
  return service_.is_session_live(active_chat_key_.toStdString());
}

QString NodeController::dmInboxToken() const {
  return QString::fromStdString(service_.dm_inbox_token_hex());
}

QString NodeController::sessionStateForKey(const QString& key) const {
  const auto state = service_.session_state(key.toStdString());
  return QString::fromUtf8(nyx_app::session_state_name(state));
}

bool NodeController::isChatSelectable(const QString& key) const {
  Q_UNUSED(key);
  // Историю можно открыть всегда; у офлайн-клиента сеть не поднимаем (см. openConversation).
  return true;
}

bool NodeController::applyRendezvousList(const QString& v) {
  const QString trimmed = v.trimmed();
  if (trimmed.isEmpty()) return false;
  if (!service_.set_rendezvous_list(trimmed.toStdString())) return false;
  rendezvous_list_ = QString::fromStdString(service_.rendezvous_list_string());
  const auto primary = service_.network_config().primary_rendezvous();
  rendezvous_ = QString::fromStdString(primary.host) + QLatin1Char(':') +
                QString::number(primary.port);
  emit rendezvousChanged();
  return true;
}

void NodeController::setRendezvous(const QString& v) {
  const QString trimmed = v.trimmed();
  if (rendezvous_ == trimmed) return;
  if (!applyRendezvousList(trimmed)) {
    network_status_ = QStringLiteral("Неверный формат rendezvous (host:port)");
    emit networkSettingsChanged();
    return;
  }
  saveNetworkSettings();
}

void NodeController::setRendezvousList(const QString& v) {
  const QString trimmed = v.trimmed();
  if (rendezvous_list_ == trimmed) return;
  if (!applyRendezvousList(trimmed)) {
    network_status_ = QStringLiteral("Неверный формат (host:port,…)");
    emit networkSettingsChanged();
    return;
  }
}

void NodeController::setDiscoveryMode(int mode) {
  if (discovery_mode_ == mode) return;
  discovery_mode_ = mode;
  service_.set_discovery_mode(mode);
  saveNetworkSettings();
}

void NodeController::saveNetworkSettings() {
  if (!service_.save_network_config()) {
    network_status_ = QStringLiteral("Не удалось сохранить");
  } else {
    network_status_ = QStringLiteral("Сохранено");
  }
  emit networkSettingsChanged();
}

bool NodeController::testRendezvousServer(const QString& hostPort) {
  QString host = hostPort;
  int port = 3478;
  const int colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    host = hostPort.left(colon);
    port = hostPort.mid(colon + 1).toInt();
  }
  const bool ok = service_.test_rendezvous(host.toStdString(), static_cast<uint16_t>(port));
  network_status_ = ok ? QStringLiteral("Rendezvous доступен") : QStringLiteral("Нет ответа");
  emit networkSettingsChanged();
  return ok;
}

void NodeController::setProfilePath(const QString& v) {
  if (profile_path_ == v) return;
  profile_path_ = v;
  service_.set_profile_path(v.toStdString());
  emit profilePathChanged();
  refreshProfile();
}

void NodeController::setNickname(const QString& v) {
  service_.set_nickname(v.trimmed().toStdString());
  refreshProfile();
}

void NodeController::setConnectionPanelOpen(bool open) {
  if (connection_panel_open_ == open) return;
  connection_panel_open_ = open;
  emit connectionPanelOpenChanged();
}

void NodeController::setGroupsDialogOpen(bool open) {
  if (groups_dialog_open_ == open) return;
  groups_dialog_open_ = open;
  emit groupsDialogOpenChanged();
}

void NodeController::setMainViewMode(int mode) {
  const int m = mode == 1 ? 1 : 0;
  if (main_view_mode_ == m) return;
  main_view_mode_ = m;
  if (m == 1) {
    if (in_chat_ && active_chat_kind_ == static_cast<int>(nyx::ConversationKind::Group) &&
        !active_chat_ref_id_.isEmpty()) {
      setFileScopeGroupId(active_chat_ref_id_);
    }
    if (!active_chat_key_.isEmpty() &&
        active_chat_key_.startsWith(QStringLiteral("group:"))) {
      service_.set_active_session(active_chat_key_.toStdString());
    }
    refreshGroupList();
    refreshFileLists();
    refreshFileAccessLists();
    if (fileExchangeReady()) refreshRemoteFileList();
  }
  emit mainViewModeChanged();
}

QString NodeController::joinFileRelPath(const QString& browseRel,
                                        const QString& entryRel) const {
  QString rel = entryRel.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const QString browse = browseRel.trimmed();
  if (browse.isEmpty()) return rel;
  if (rel.isEmpty()) return browse;
  if (rel.startsWith(browse + QLatin1Char('/'))) return rel;
  return browse + QLatin1Char('/') + rel;
}

uint32_t NodeController::filePermissionsAt(const QString& rootPath,
                                           const QString& relativePath) const {
  if (file_scope_group_id_.isEmpty()) return nyx::kFilePermissionAll;
  if (isFileScopeOwner()) return nyx::kFilePermissionAll;
  const QString root = resolveAccessRootPath(rootPath);
  return service_.my_file_permissions(file_scope_group_id_.toStdString(),
                                      root.toStdString(), relativePath.toStdString());
}

uint32_t NodeController::currentFilePermissions() const {
  if (file_scope_group_id_.isEmpty()) return nyx::kFilePermissionAll;
  if (isFileScopeOwner()) return nyx::kFilePermissionAll;

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

bool NodeController::canFileList() const {
  if (file_scope_group_id_.isEmpty()) return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::List));
}

bool NodeController::hasFilePermission(int permissionBit) const {
  return nyx::FileAccessStore::has_permission(currentFilePermissions(),
                                              static_cast<nyx::FilePermission>(permissionBit));
}

bool NodeController::isFileScopeOwner() const {
  if (file_scope_group_id_.isEmpty()) return false;
  const QString scope = file_scope_group_id_.trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString().trimmed().toLower() != scope) continue;
    return m.value(QStringLiteral("isOwner")).toBool();
  }
  return false;
}

bool NodeController::canManageFileRoles() const {
  if (file_scope_group_id_.isEmpty()) return false;
  if (isFileScopeOwner()) return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageRoles));
}

bool NodeController::canFileUpload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Upload));
}

bool NodeController::canFileDownload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Download));
}

bool NodeController::canFileDownloadAt(const QString& rootPath, const QString& relativePath) const {
  if (file_scope_group_id_.isEmpty()) return true;
  if (isFileScopeOwner()) return true;
  return nyx::FileAccessStore::has_permission(
      filePermissionsAt(rootPath, relativePath), nyx::FilePermission::Download);
}

bool NodeController::canDownloadFolderAt(const QString& rootPath,
                                         const QString& relativePath) const {
  if (canFileDownloadAt(rootPath, relativePath)) return true;
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
  if (file_scope_group_id_.isEmpty()) return true;
  if (isFileScopeOwner()) return true;
  return nyx::FileAccessStore::has_permission(
      filePermissionsAt(rootPath, relativePath), nyx::FilePermission::OpenRemote);
}

bool NodeController::canManageFileShares() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageShares));
}

bool NodeController::canRemoveShareFolder() const {
  if (file_scope_group_id_.isEmpty()) return true;
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != file_scope_group_id_) continue;
    if (m.value(QStringLiteral("isOwner")).toBool()) return true;
    break;
  }
  return canManageFileShares();
}

bool NodeController::canAddShareFolder() const {
  if (file_scope_group_id_.isEmpty()) return true;
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
    if (gid != scope) continue;
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
  if (file_scope_group_id_.isEmpty() || file_access_target_root_.isEmpty()) return;

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  const std::string root_norm =
      nyx::normalize_grant_root(file_access_target_root_.toStdString());
  const std::string rel_posix = file_access_target_rel_.toStdString();
  auto rel_posix_norm = rel_posix;
  for (char& c : rel_posix_norm) {
    if (c == '\\') c = '/';
  }

  auto find_grant = [&](const std::string& rel, const nyx::UserId& user) -> const nyx::FileRootGrant* {
    for (const auto& g : policy.root_grants) {
      if (nyx::normalize_grant_root(g.root_path) != root_norm) continue;
      std::string gr = g.relative_path;
      for (char& c : gr) {
        if (c == '\\') c = '/';
      }
      if (gr != rel) continue;
      if (g.user_id != user) continue;
      return &g;
    }
    return nullptr;
  };

  for (const QVariant& mv : file_member_access_) {
    const QVariantMap mm = mv.toMap();
    if (mm.value(QStringLiteral("isOwner")).toBool()) continue;
    const QString uid = mm.value(QStringLiteral("userId")).toString();
    nyx::UserId user{};
    nyx::ByteBuffer buf;
    if (!nyx::from_hex(uid.toStdString(), buf) || buf.size() != user.size()) continue;
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
      walk = slash == std::string::npos ? std::string{} : walk.substr(0, slash);
      if (const nyx::FileRootGrant* anc = find_grant(walk, user)) {
        if (!anc->direct_only && !anc->role_id.empty()) {
          grantMode = QStringLiteral("inherited");
          role_id = utf8q(anc->role_id);
          inherited_from = walk.empty() ? QStringLiteral("корень") : utf8q(walk);
          break;
        }
      }
      if (walk.empty()) break;
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
  if (file_scope_group_id_.isEmpty() || file_access_target_root_.isEmpty()) return;

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  const std::string root_norm =
      nyx::normalize_grant_root(file_access_target_root_.toStdString());
  std::string rel_posix_norm = file_access_target_rel_.toStdString();
  for (char& c : rel_posix_norm) {
    if (c == '\\') c = '/';
  }

  const nyx::UserId wildcard = nyx::FileAccessStore::path_role_user();
  auto find_wildcard = [&](const std::string& rel) -> const nyx::FileRootGrant* {
    for (const auto& g : policy.root_grants) {
      if (nyx::normalize_grant_root(g.root_path) != root_norm) continue;
      std::string gr = g.relative_path;
      for (char& c : gr) {
        if (c == '\\') c = '/';
      }
      if (gr != rel) continue;
      if (g.user_id != wildcard) continue;
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
    walk = slash == std::string::npos ? std::string{} : walk.substr(0, slash);
    if (const nyx::FileRootGrant* anc = find_wildcard(walk)) {
      if (!anc->direct_only && !anc->role_id.empty()) {
        file_path_role_id_ = utf8q(anc->role_id);
        file_path_role_inherited_from_ =
            walk.empty() ? QStringLiteral("корень") : utf8q(walk);
        return;
      }
    }
    if (walk.empty()) break;
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

void NodeController::syncFileAccessTargetFromBrowse() {
  file_access_target_root_ = file_selected_share_root_;
  file_access_target_rel_ = file_browse_path_;
  updateFileAccessTargetLabel();
  refreshFilePathMemberAccess();
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
  if (p.startsWith(QStringLiteral("file:///"))) p = QUrl(p).toLocalFile();
  if (p.isEmpty()) return p;
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(p, QString::fromStdString(r.path))) {
      return QString::fromStdString(r.path);
    }
  }
  return normalizeShareRootPath(p);
}

void NodeController::openAccessForPath(const QString& rootPath, const QString& relativePath,
                                       const QString& label) {
  setFileAccessTarget(rootPath, relativePath);
  if (!label.trimmed().isEmpty()) file_access_target_label_ = label.trimmed();
  emit fileAccessChanged();
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
  if (!service_.set_path_member_file_role(
          file_scope_group_id_.toStdString(), file_access_target_root_.toStdString(),
          file_access_target_rel_.toStdString(), userIdHex.toStdString(),
          roleId.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Права на объект обновлены"));
}

void NodeController::setPathGrantDirect(const QString& userIdHex) {
  if (!canManageFileRoles()) return;
  const int perms = static_cast<int>(nyx::FilePermission::List) |
                    static_cast<int>(nyx::FilePermission::Download);
  if (!service_.set_path_direct_file_permissions(
          file_scope_group_id_.toStdString(), file_access_target_root_.toStdString(),
          file_access_target_rel_.toStdString(), userIdHex.toStdString(),
          static_cast<uint32_t>(perms))) {
    showToast(QStringLiteral("Не удалось задать прямые права"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::clearPathMemberGrant(const QString& userIdHex) {
  if (!canManageFileRoles()) return;
  service_.set_path_member_file_role(file_scope_group_id_.toStdString(),
                                     file_access_target_root_.toStdString(),
                                     file_access_target_rel_.toStdString(),
                                     userIdHex.toStdString(), {});
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
                              file_access_target_rel_.toStdString(), roleId.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль"), true);
    return;
  }
  refreshFileAccessLists();
  showToast(QStringLiteral("Роль на объект обновлена"));
}

void NodeController::clearPathRole() { setPathRole({}); }

void NodeController::createPermissionPreset(const QString& name, int permissions) {
  if (!canManageFileRoles()) return;
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty()) return;
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
  if (!canManageFileRoles()) return;
  if (!service_.remove_permission_preset(file_scope_group_id_.toStdString(),
                                         presetId.toStdString())) {
    showToast(QStringLiteral("Не удалось удалить пресет"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::togglePermissionPresetBit(const QString& presetId, int permissionBit) {
  if (!canManageFileRoles()) return;
  for (const QVariant& pv : file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId) continue;
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
  if (!canManageFileRoles() || !canEditFileRolePermissions(roleId)) return;
  int perms = 0;
  QString preset_name;
  for (const QVariant& pv : file_permission_preset_list_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("presetId")).toString() != presetId) continue;
    perms = pm.value(QStringLiteral("permissions")).toInt();
    preset_name = pm.value(QStringLiteral("name")).toString();
    break;
  }
  for (const QVariant& rv : file_role_list_) {
    const QVariantMap rm = rv.toMap();
    if (rm.value(QStringLiteral("roleId")).toString() != roleId) continue;
    updateFileRole(roleId, rm.value(QStringLiteral("name")).toString(), perms);
    showToast(QStringLiteral("К роли применён пресет «") + preset_name + QStringLiteral("»"));
    return;
  }
}

void NodeController::togglePathDirectPermission(const QString& userIdHex, int permissionBit) {
  if (!canManageFileRoles()) return;
  int perms = static_cast<int>(nyx::FilePermission::List) |
              static_cast<int>(nyx::FilePermission::Download);
  for (const QVariant& pv : file_path_member_access_) {
    const QVariantMap pm = pv.toMap();
    if (pm.value(QStringLiteral("userId")).toString() != userIdHex) continue;
    if (pm.value(QStringLiteral("grantMode")).toString() == QStringLiteral("direct")) {
      perms = pm.value(QStringLiteral("directPermissions")).toInt();
    }
    break;
  }
  perms ^= permissionBit;
  if (!service_.set_path_direct_file_permissions(
          file_scope_group_id_.toStdString(), file_access_target_root_.toStdString(),
          file_access_target_rel_.toStdString(), userIdHex.toStdString(),
          static_cast<uint32_t>(perms))) {
    showToast(QStringLiteral("Не удалось обновить права"), true);
    return;
  }
  refreshFileAccessLists();
}

void NodeController::refreshFileAccess() { refreshFileAccessLists(); }

void NodeController::refreshFieldRoster() {
  refreshGroupList();
  refreshFileAccessLists();
}

void NodeController::setMemberFileRole(const QString& userIdHex, const QString& roleId) {
  if (!canManageFileRoles()) {
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  if (!service_.set_member_file_role(file_scope_group_id_.toStdString(),
                                     userIdHex.toStdString(), roleId.toStdString())) {
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
      !service_.upsert_file_role(file_scope_group_id_.toStdString(), role)) {
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
  if (files_section_ == 1) refreshRemoteFileModel();
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
    if (rm.value(QStringLiteral("roleId")).toString() != roleId) continue;
    int perms = rm.value(QStringLiteral("permissions")).toInt();
    perms ^= permissionBit;
    updateFileRole(roleId, rm.value(QStringLiteral("name")).toString(), perms);
    return;
  }
}

void NodeController::openRemoteFile(const QString& hashHex, const QString& fileName,
                                    const QString& rootPath, const QString& relativePath) {
  if (!canFileOpenRemoteAt(rootPath, relativePath)) {
    showToast(QStringLiteral("Нет права открывать файлы по сети"));
    return;
  }
  const QString suggested = fileName.trimmed().isEmpty() ? QStringLiteral("download") : fileName.trimmed();
  const QString dest = pickSaveFile(suggested);
  if (dest.isEmpty()) return;
  if (!service_.download_file(hashHex.trimmed().toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Не удалось запросить файл"));
    return;
  }
  showToast(QStringLiteral("Запрос файла отправлен…"));
}

void NodeController::openFilesView() { setMainViewMode(1); }

void NodeController::showChatView() { setMainViewMode(0); }

void NodeController::openFilesDialog() { openFilesView(); }

void NodeController::setFileScopeGroupId(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (file_scope_group_id_ == gid) return;
  file_scope_group_id_ = gid;
  file_selected_share_root_.clear();
  resetFileBrowse();
  syncFileScopeLabel();
  service_.save_files_scope_group_id(gid.toStdString());
  service_.save_files_selected_root({});
  if (!gid.isEmpty()) {
    service_.set_active_session(
        QStringLiteral("group:%1").arg(gid).toStdString());
  }
  refreshFileLists();
  refreshFileAccessLists();
  if (fileExchangeReady()) refreshRemoteFileList();
  emit filesChanged();
  emit fileAccessChanged();
}

void NodeController::openGroupsDialog() {
  refreshGroupList();
  setGroupsDialogOpen(true);
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

QVariantList NodeController::entriesToVariant(const std::vector<nyx::FileEntry>& entries,
                                              bool remote) const {
  QVariantList list;
  const QString browse_rel = remote ? file_remote_browse_path_ : file_browse_path_;
  for (const auto& e : entries) {
    QVariantMap m;
    const std::string leaf = e.leaf_name();
    const std::string rel = e.relative_path;
    QString display = utf8q(leaf);
    if (display.isEmpty() && !rel.empty()) display = utf8q(rel);
    const bool is_dir = e.is_directory();
    QString full_rel;
    if (is_dir) {
      full_rel = utf8q(rel);
      full_rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    } else {
      full_rel = joinFileRelPath(browse_rel, utf8q(rel));
    }
    const QString root = utf8q(e.root_path);
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
    if (remote) {
      const bool can_dl = is_dir ? canDownloadFolderAt(root, full_rel)
                                 : canFileDownloadAt(root, full_rel);
      m.insert(QStringLiteral("canDownload"), can_dl);
      m.insert(QStringLiteral("canOpenRemote"),
                !is_dir && canFileOpenRemoteAt(root, full_rel));
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
  if (p.isEmpty()) return p;
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

QString NodeController::scopeLabelForGroupId(const QString& groupIdHex) const {
  if (groupIdHex.isEmpty()) return QStringLiteral("Личные");
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == groupIdHex) {
      return m.value(QStringLiteral("name")).toString();
    }
  }
  return groupIdHex.left(8) + QStringLiteral("…");
}

bool NodeController::canRemoveShareRoot(const nyx::ShareRoot& root) const {
  if (root.is_personal()) return true;
  const QString gid =
      QString::fromStdString(nyx::FileIndex::group_id_hex(root.group_id)).trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid) continue;
    if (m.value(QStringLiteral("isOwner")).toBool()) return true;
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
  if (roots.empty()) return;

  if (!file_selected_share_root_.isEmpty()) {
    for (const auto& r : roots) {
      const QString path = QString::fromStdString(r.path);
      if (!shareRootPathsEqual(file_selected_share_root_, path)) continue;
      file_selected_share_root_ = path;
      const QString gid = r.is_personal()
                              ? QString()
                              : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id))
                                    .trimmed()
                                    .toLower();
      if (file_scope_group_id_ != gid) {
        file_scope_group_id_ = gid;
        service_.save_files_scope_group_id(gid.toStdString());
      }
      service_.save_files_selected_root(path.toStdString());
      return;
    }
    file_selected_share_root_.clear();
    service_.save_files_selected_root({});
  }

  const auto scope_roots =
      service_.share_roots_for_scope(file_scope_group_id_.toStdString());
  if (!scope_roots.empty()) return;

  const nyx::ShareRoot& first = roots.front();
  const QString gid = first.is_personal()
                          ? QString()
                          : QString::fromStdString(nyx::FileIndex::group_id_hex(first.group_id))
                                .trimmed()
                                .toLower();
  file_scope_group_id_ = gid;
  service_.save_files_scope_group_id(gid.toStdString());
}

void NodeController::setFilesSection(int section) {
  if (section < 0) section = 0;
  if (section > 2) section = 2;
  if (files_section_ == section && section != 2) return;
  files_section_ = section;
  if (files_section_ == 1 && fileExchangeReady()) refreshRemoteFileList();
  if (files_section_ == 2) {
    refreshGroupList();
    refreshFileAccessLists();
  }
  emit filesChanged();
  emit fileAccessChanged();
}

std::vector<nyx::FileEntry> NodeController::remoteRootsCatalog(
    const std::vector<nyx::FileEntry>& all) const {
  nyx::GroupId scope{};
  if (!file_scope_group_id_.isEmpty()) {
    nyx::FileIndex::group_id_from_hex(file_scope_group_id_.toStdString(), scope);
  }

  std::map<std::string, int> file_counts;
  for (const auto& e : all) {
    if (e.is_directory()) continue;
    file_counts[nyx::normalize_utf8_path(e.root_path)]++;
  }

  // Сначала маркеры папок с hub (в т.ч. пустые share-корни с 0 файлов).
  std::map<std::string, nyx::FileEntry> by_root;
  for (const auto& e : all) {
    if (!e.is_directory()) continue;
    const std::string norm = nyx::normalize_utf8_path(e.root_path);
    nyx::FileEntry marker = e;
    marker.root_path = norm;
    const auto it = file_counts.find(norm);
    if (it != file_counts.end()) marker.size = static_cast<uint64_t>(it->second);
    by_root[norm] = std::move(marker);
  }

  // Корни только из файлов (на случай ответа без маркеров).
  for (const auto& [path, count] : file_counts) {
    if (by_root.count(path)) continue;
    nyx::ShareRoot sr;
    sr.path = path;
    sr.group_id = scope;
    by_root[path] = nyx::FileIndex::make_directory_marker(sr, count);
  }

  std::vector<nyx::FileEntry> out;
  out.reserve(by_root.size());
  for (auto& [_, entry] : by_root) out.push_back(std::move(entry));
  return out;
}

void NodeController::syncFileBrowseCrumbs() {
  file_browse_crumbs_.clear();
  if (file_selected_share_root_.isEmpty()) return;

  const QFileInfo rootInfo(file_selected_share_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"), rootInfo.fileName().isEmpty()
                                              ? file_selected_share_root_
                                              : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  file_browse_crumbs_.append(rootCrumb);

  QString rel = file_browse_path_;
  if (rel.isEmpty()) return;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  int from = 0;
  QString built;
  while (from < rel.length()) {
    const int slash = rel.indexOf(QLatin1Char('/'), from);
    const QString segment =
        slash < 0 ? rel.mid(from) : rel.mid(from, slash - from);
    if (!segment.isEmpty()) {
      built = built.isEmpty() ? segment : built + QLatin1Char('/') + segment;
      QVariantMap crumb;
      crumb.insert(QStringLiteral("label"), segment);
      crumb.insert(QStringLiteral("path"), built);
      file_browse_crumbs_.append(crumb);
    }
    if (slash < 0) break;
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

  if (file_resources_root_.isEmpty()) return;

  const QFileInfo rootInfo(file_resources_root_);
  QVariantMap rootCrumb;
  rootCrumb.insert(QStringLiteral("label"), rootInfo.fileName().isEmpty() ? file_resources_root_
                                                                         : rootInfo.fileName());
  rootCrumb.insert(QStringLiteral("path"), QString());
  rootCrumb.insert(QStringLiteral("isRoots"), false);
  file_remote_browse_crumbs_.append(rootCrumb);

  QString rel = file_remote_browse_path_;
  if (rel.isEmpty()) return;
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
    if (slash < 0) break;
    from = slash + 1;
  }
}

void NodeController::setFileSelectedShareRoot(const QString& path) {
  const QString p = path.trimmed();
  if (p.isEmpty()) return;

  QString canonical;
  QString gid = file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    const QString rp = QString::fromStdString(r.path);
    if (!shareRootPathsEqual(p, rp)) continue;
    canonical = rp;
    gid = r.is_personal()
              ? QString()
              : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    break;
  }
  if (canonical.isEmpty()) return;
  if (file_selected_share_root_ == canonical) return;

  if (file_scope_group_id_ != gid) {
    file_scope_group_id_ = gid;
    syncFileScopeLabel();
    service_.save_files_scope_group_id(gid.toStdString());
    refreshFileAccessLists();
    emit fileAccessChanged();
  }
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
      if (root.isEmpty()) root = navPath.trimmed();
      if (root.isEmpty()) return;
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
    // Подгрузить текущий уровень с hub (не весь node_modules разом).
    service_.request_remote_files_at(file_scope_group_id_.toStdString(),
                                     file_resources_root_.toStdString(),
                                     file_remote_browse_path_.toStdString());
    refreshRemoteFileModel();
    emit filesChanged();
    return;
  }

  if (navPath.trimmed().isEmpty()) return;
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

  // Над share-корнем: снять выбор папки (список «Мои папки» остаётся слева).
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
    if (index < 0 || index >= file_remote_browse_crumbs_.size()) return;
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

  if (index < 0 || index >= file_browse_crumbs_.size()) return;
  file_browse_path_ = file_browse_crumbs_.at(index).toMap().value(QStringLiteral("path")).toString();
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  emit filesChanged();
}

void NodeController::addDroppedUrls(const QVariantList& urls) {
  if (urls.isEmpty()) return;
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
    if (p.startsWith(QStringLiteral("file:///"))) p = QUrl(p).toLocalFile();
    if (p.isEmpty()) continue;
    QFileInfo info(p);
    if (info.isDir()) dirs.push_back(info.absoluteFilePath());
    else if (info.isFile()) dirs.push_back(info.absolutePath());
  }
  if (dirs.isEmpty()) {
    showToast(QStringLiteral("Не удалось добавить из перетаскивания"));
    return;
  }
  runIndexJob(dirs.front(), file_scope_group_id_, false);
  for (int i = 1; i < dirs.size(); ++i) {
    // Последовательно: следующие папки после завершения первой через очередь не делаем —
    // пользователь может добавить ещё раз. Одна крупная папка за раз достаточно.
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
  const auto roots = service_.all_share_roots();
  for (const auto& r : roots) {
    const QString path = QString::fromStdString(r.path);
    const QString scopeId = r.is_personal()
                                ? QString()
                                : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id))
                                      .trimmed()
                                      .toLower();
    QVariantMap m;
    m.insert(QStringLiteral("path"), path);
    const QFileInfo fi(path);
    m.insert(QStringLiteral("displayName"),
             fi.fileName().isEmpty() ? path : fi.fileName());
    m.insert(QStringLiteral("isPersonal"), r.is_personal());
    m.insert(QStringLiteral("scopeGroupId"), scopeId);
    m.insert(QStringLiteral("scopeLabel"), scopeLabelForGroupId(scopeId));
    m.insert(QStringLiteral("fileCount"), service_.file_count_in_root(r.path));
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
    if (!found) file_selected_share_root_.clear();
  }
  if (file_selected_share_root_.isEmpty() && !file_share_roots_.isEmpty()) {
    file_selected_share_root_ =
        file_share_roots_.first().toMap().value(QStringLiteral("path")).toString();
    resetFileBrowse();
  }
}

void NodeController::refreshLocalFileModel() {
  if (file_selected_share_root_.isEmpty()) {
    local_file_list_.clear();
    return;
  }
  std::string root_path = file_selected_share_root_.toStdString();
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(file_selected_share_root_, QString::fromStdString(r.path))) {
      root_path = r.path;
      break;
    }
  }
  const auto entries = service_.local_files_at_root(root_path, file_browse_path_.toStdString());
  local_file_list_ = entriesToVariant(entries, false);
}

void NodeController::reconcileRemoteBrowsePath(const std::vector<nyx::FileEntry>& catalog) {
  if (file_resources_root_.isEmpty()) return;
  bool found = false;
  for (const auto& e : catalog) {
    if (e.root_path.empty()) continue;
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
  const QString dir =
      QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Выберите папку для индекса"),
                                        QDir::homePath());
  return dir;
}

QString NodeController::pickSaveFile(const QString& suggestedFileName) {
  const QString downloads =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString base = downloads.isEmpty() ? QDir::homePath() : downloads;
  const QString name = suggestedFileName.trimmed().isEmpty()
                           ? QStringLiteral("download")
                           : suggestedFileName.trimmed();
  const QString suggested = QDir(base).filePath(name);
  return QFileDialog::getSaveFileName(nullptr, QStringLiteral("Сохранить файл"), suggested,
                                      QStringLiteral("Все файлы (*.*)"));
}

QString NodeController::pickSaveFolder() {
  const QString downloads =
      QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const QString start = downloads.isEmpty() ? QDir::homePath() : downloads;
  return QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Выберите папку для сохранения"),
                                           start);
}

void NodeController::runIndexJob(const QString& path, const QString& scopeGroupId, bool rescan) {
  const QString p = path.trimmed();
  if (p.isEmpty()) return;
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
  std::thread([this, p, scope, rescan]() {
    const bool ok = rescan ? service_.rescan_share_root(p.toStdString(), scope.toStdString())
                           : service_.index_folder(p.toStdString(), scope.toStdString());
    const int count = ok ? service_.file_count_in_root(p.toStdString()) : 0;
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
  }).detach();
}

void NodeController::addIndexedFolder(const QString& path) {
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  QString p = path.trimmed();
  if (p.isEmpty()) {
    p = pickFolder();
    if (p.isEmpty()) return;
  }
  if (p.startsWith(QStringLiteral("file:///"))) p = QUrl(p).toLocalFile();
  runIndexJob(p, file_scope_group_id_, false);
}

void NodeController::removeIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty()) return;

  QString scope = file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path))) continue;
    scope = r.is_personal()
                ? QString()
                : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
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
  if (files_section_ == 1 && fileExchangeReady()) refreshRemoteFileList();
  showToast(QStringLiteral("Папка убрана из индекса"));
}

void NodeController::rescanIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty()) return;
  QString scope = file_scope_group_id_;
  for (const auto& r : service_.all_share_roots()) {
    if (!shareRootPathsEqual(path, QString::fromStdString(r.path))) continue;
    scope = r.is_personal()
                ? QString()
                : QString::fromStdString(nyx::FileIndex::group_id_hex(r.group_id)).trimmed().toLower();
    break;
  }
  runIndexJob(path.trimmed(), scope, true);
}

void NodeController::refreshRemoteFileList() {
  // Полный сброс browse при обновлении корней — иначе остаёмся внутри удалённой папки.
  file_resources_root_.clear();
  file_remote_browse_path_.clear();
  syncRemoteBrowseCrumbs();
  if (!service_.request_remote_files_at(file_scope_group_id_.toStdString(), {}, {})) {
    refreshRemoteFileModel();
    emit filesChanged();
    showToast(fileExchangeHint().isEmpty()
                  ? QStringLiteral("Не удалось запросить файлы")
                  : fileExchangeHint());
    return;
  }
  emit filesChanged();
}

void NodeController::downloadFile(const QString& hashHex, const QString& fileName,
                                  const QString& rootPath, const QString& relativePath) {
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
  const QString suggested = fileName.trimmed().isEmpty() ? QStringLiteral("download") : fileName.trimmed();
  const QString dest = pickSaveFile(suggested);
  if (dest.isEmpty()) return;
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
  if (destDir.isEmpty()) return;
  const QString canonical = resolveAccessRootPath(rootPath);
  std::string root = canonical.toStdString();
  for (const auto& e : service_.remote_files()) {
    if (!shareRootPathsEqual(canonical, QString::fromStdString(e.root_path))) continue;
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

void NodeController::wireCallbacks() {
  service_.set_on_status([this](const std::string& text) {
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
          const QString q = QString::fromStdString(text);
          setStatus(q);
          const QString lower = q.toLower();
          if (lower.contains(QStringLiteral("вошёл")) ||
              lower.contains(QStringLiteral("вошел"))) {
            refreshGroupList();
            if (main_view_mode_ == 1) refreshFileAccessLists();
          }
          // Lookup/rendezvous/register — только в статус-бар, не в тосты (reconnect спамит).
          const bool progress_noise =
              lower.contains(QStringLiteral("lookup")) ||
              lower.contains(QStringLiteral("rendezvous")) ||
              lower.contains(QStringLiteral("register")) ||
              lower.contains(QStringLiteral("handshake")) ||
              lower.contains(QStringLiteral("bind ")) ||
              lower.startsWith(QStringLiteral("hub «")) ||
              lower.contains(QStringLiteral("invite:")) ||
              lower.contains(QStringLiteral("подключение к hub"));
          if (!progress_noise &&
              (lower.contains(QStringLiteral("failed")) ||
               lower.contains(QStringLiteral("не удалось")) ||
               lower.contains(QStringLiteral("неверн")) ||
               lower.contains(QStringLiteral("отказ")) ||
               lower.contains(QStringLiteral("файл сохранён")) ||
               lower.contains(QStringLiteral("приём")) ||
               lower.contains(QStringLiteral("запрос файла")) ||
               lower.contains(QStringLiteral("timeout")) ||
               lower.contains(QStringLiteral("поле недоступно")))) {
            showToast(q, lower.contains(QStringLiteral("отказ")) ||
                             lower.contains(QStringLiteral("failed")) ||
                             lower.contains(QStringLiteral("не удалось")));
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_invite_token([this](const std::string& hex) {
    QMetaObject::invokeMethod(
        this,
        [this, hex]() {
          invite_token_ = QString::fromStdString(hex);
          emit inviteTokenChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_message([this](const nyx_app::UiMessage& msg) {
    QMetaObject::invokeMethod(
        this,
        [this, msg]() {
          const QString chat_key = QString::fromStdString(msg.chat_key);
          const bool for_active =
              chat_key.isEmpty() || chat_key == active_chat_key_ ||
              (msg.session_id == service_.active_session_id());
          if (for_active) {
            messages_.appendMessage(QString::fromStdString(msg.author),
                                    QString::fromStdString(msg.text), msg.outgoing,
                                    msg.timestamp_ms);
          } else if (!msg.outgoing && !chat_key.isEmpty()) {
            chat_list_.bumpUnread(chat_key);
          }
          refreshChatList();
          if (!msg.outgoing) {
            const QString author = QString::fromStdString(msg.author);
            const QString preview = QString::fromStdString(msg.text);
            emit incomingMessage(author, preview);
            if (!window_active_ && tray_icon_) {
              tray_icon_->showMessage(
                  author,
                  preview.length() > 120 ? preview.left(117) + QStringLiteral("...") : preview,
                  QSystemTrayIcon::Information, 4000);
            }
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_chat_ready([this](const std::string& session_id, const std::string& peer_title,
                                    const std::string& conn_label, nyx::ConversationKind kind,
                                    const std::string& ref_id) {
    QMetaObject::invokeMethod(
        this,
        [this, session_id, peer_title, conn_label, kind, ref_id]() {
          const QString sid = QString::fromStdString(session_id);
          const QString ref = QString::fromStdString(ref_id);
          const QString list_key =
              kind == nyx::ConversationKind::Group
                  ? (QStringLiteral("group:") + ref)
                  : (ref.isEmpty() ? sid : QStringLiteral("dm:") + ref);

          chat_list_.setSessionState(list_key, QStringLiteral("live"));
          if (sid != list_key)
            chat_list_.setSessionState(sid, QStringLiteral("live"));

          const bool user_waiting = pending_field_join_notify_;
          const bool was_selected = !active_chat_key_.isEmpty() && active_chat_key_ == list_key;
          pending_field_join_notify_ = false;

          // Не красть active_session / UI у другого открытого чата.
          if (user_waiting || was_selected) {
            service_.set_active_session(session_id);
            active_chat_kind_ = static_cast<int>(kind);
            active_chat_ref_id_ = ref;
            enterChat(QString::fromStdString(peer_title), QString::fromStdString(conn_label),
                      static_cast<int>(kind), ref);
            if (kind == nyx::ConversationKind::Group) {
              loadStoredHistory(static_cast<int>(kind), ref, list_key);
              showToast(QStringLiteral("В поле «") + QString::fromStdString(peer_title) +
                        QStringLiteral("»"));
            }
          } else if (kind == nyx::ConversationKind::Group) {
            showToast(QStringLiteral("Поле «") + QString::fromStdString(peer_title) +
                      QStringLiteral("» снова в сети"));
          } else {
            showToast(QStringLiteral("Собеседник снова в сети"));
          }

          refreshChatList();
          refreshGroupList();
          emit sessionsChanged();
          emit chatChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_session_ended([this](const std::string& session_id) {
    QMetaObject::invokeMethod(
        this,
        [this, session_id]() {
          const QString sid = QString::fromStdString(session_id);
          if (!sid.isEmpty()) {
            chat_list_.setSessionState(sid, QStringLiteral("offline"));
          }
          const bool active_gone =
              !active_chat_key_.isEmpty() &&
              !service_.is_session_live(active_chat_key_.toStdString());
          const bool ended_active =
              !active_chat_key_.isEmpty() &&
              (sid == active_chat_key_ ||
               (active_gone && (sid.startsWith(QStringLiteral("group:join:")) ||
                                sid.startsWith(QStringLiteral("dm:")))));
          if (ended_active || (in_chat_ && active_gone)) {
            chat_list_.setSessionState(active_chat_key_, QStringLiteral("offline"));
            endLiveSession();
          }
          refreshGroupList();
          refreshChatList();
          emit sessionsChanged();
          emit chatChanged();
          if (pending_field_join_notify_) {
            pending_field_join_notify_ = false;
            const auto st = service_.session_state(session_id);
            if (st == nyx_app::SessionState::Offline) {
              showToast(QStringLiteral("Владелец поля не в сети"), false);
            }
          }
          // Auto-retry только при неожиданном Offline и включённом intent.
          // Disconnected = пользователь нажал «Отключиться» — не переподключать.
          if (sid.startsWith(QStringLiteral("group:")) &&
              !sid.startsWith(QStringLiteral("group:join:"))) {
            const auto st = service_.session_state(session_id);
            if (st == nyx_app::SessionState::Offline &&
                service_.is_session_intent_enabled(sid.toStdString())) {
              QTimer::singleShot(3000, this, [this, sid]() {
                if (!auto_start_owned_hub_) return;
                if (!service_.is_session_intent_enabled(sid.toStdString())) return;
                if (service_.is_session_up(sid.toStdString())) return;
                service_.ensure_session(sid.toStdString());
                refreshChatList();
                emit sessionsChanged();
                emit chatChanged();
              });
            }
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_sessions_changed([this]() {
    QMetaObject::invokeMethod(this, [this]() {
      refreshChatList();
      emit sessionsChanged();
      emit busyChanged();
      emit listeningChanged();
    }, Qt::QueuedConnection);
  });

  service_.set_on_lan_peers([this](const std::vector<nyx::LanPeer>& peers) {
    QMetaObject::invokeMethod(
        this,
        [this, peers]() {
          QVariantList list;
          for (const auto& p : peers) {
            QVariantMap m;
            m.insert("instance", QString::fromStdString(p.instance));
            m.insert("host", QString::fromStdString(p.host));
            m.insert("port", static_cast<int>(p.port));
            m.insert("userId", QString::fromStdString(p.user_id_short));
            list.append(m);
          }
          lan_peers_.setPeers(list);
          emit listeningChanged();
          emit busyChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_mode([this](nyx_app::NodeMode) {
    QMetaObject::invokeMethod(
        this, [this]() { emit listeningChanged(); emit busyChanged(); }, Qt::QueuedConnection);
  });

  service_.set_on_group_created([this](const std::string& gid, const std::string& invite) {
    QMetaObject::invokeMethod(
        this,
        [this, gid, invite]() {
          last_group_invite_ = QString::fromStdString(invite);
          emit lastGroupInviteChanged();
          setStatus(QString("поле создано\n  id: %1\n  invite: %2")
                        .arg(QString::fromStdString(gid), last_group_invite_));
          refreshChatList();
          refreshGroupList();
          if (service_.auto_start_owned_hub()) {
            startFieldHub(QString::fromStdString(gid));
            toast_ = QStringLiteral("Поле создано — hub запускается автоматически");
          } else {
            toast_ = QStringLiteral("Поле создано — нажмите «Запустить hub» в списке полей");
          }
          emit toastChanged();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_file_progress([this](const std::string& label, int percent) {
    QMetaObject::invokeMethod(
        this,
        [this, label, percent]() {
          file_progress_label_ = QString::fromStdString(label);
          file_progress_percent_ = percent;
          file_progress_visible_ = percent > 0 && percent < 100;
          emit fileProgressChanged();
          if (percent >= 100) {
            QTimer::singleShot(1500, this, [this]() {
              file_progress_visible_ = false;
              emit fileProgressChanged();
            });
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_file_index_progress(
      [this](const std::string& path, int files_scanned, bool finished) {
        QMetaObject::invokeMethod(
            this,
            [this, path, files_scanned, finished]() {
              file_index_files_scanned_ = files_scanned;
              if (finished) return;  // финал рисует runIndexJob
              file_index_progress_visible_ = true;
              // Плавный индикатор без известного total (не «зависание»).
              const int paced = 5 + static_cast<int>(
                  (95.0 * (1.0 - std::exp(-static_cast<double>(files_scanned) / 80.0))));
              file_index_progress_percent_ = qBound(5, paced, 95);
              const QString name = QString::fromStdString(path);
              file_index_progress_label_ =
                  name.isEmpty()
                      ? QStringLiteral("Сканирование… %1 файлов").arg(files_scanned)
                      : QStringLiteral("%1 · %2").arg(name).arg(files_scanned);
              emit fileIndexProgressChanged();
            },
            Qt::QueuedConnection);
      });

  service_.set_on_remote_files([this](const std::vector<nyx::FileEntry>& entries) {
    QMetaObject::invokeMethod(
        this,
        [this, entries]() {
          refreshRemoteFileModel(entries);
          emit filesChanged();
          const int n = static_cast<int>(remote_file_list_.size());
          if (file_resources_root_.isEmpty()) {
            showToast(n == 0 ? QStringLiteral("Ресурсы поля: папок нет")
                             : QStringLiteral("Ресурсы поля: %1 папок").arg(n));
          } else {
            showToast(QStringLiteral("Уровень каталога: %1").arg(n));
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_file_access_sync([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshFileAccessLists();
          refreshRemoteFileModel();
          emit filesChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::refreshAccountList() {
  account_list_.clear();
  const auto accounts = nyx::list_accounts();
  for (const auto& a : accounts) {
    QVariantMap m;
    m.insert(QStringLiteral("id"), QString::fromStdString(a.id));
    m.insert(QStringLiteral("nickname"), QString::fromStdString(a.nickname));
    m.insert(QStringLiteral("idShort"), QString::fromStdString(a.id.substr(0, 8)));
    m.insert(QStringLiteral("locked"), a.locked);
    m.insert(QStringLiteral("hasRecovery"), a.has_recovery);
    m.insert(QStringLiteral("rememberActive"), a.remember_active);
    account_list_.append(m);
  }
  last_account_id_ = QString::fromStdString(nyx::last_account_id());
  emit accountGateChanged();
}

void NodeController::finishAccountUnlock(bool begin_session) {
  account_gate_error_.clear();
  last_account_id_ = QString::fromStdString(nyx::active_account_id());
  if (begin_session) {
    session_unlocked_ = true;
    emit sessionUnlockedChanged();
    beginMainSession();
  }
  refreshAccountList();
}

bool NodeController::createAccount(const QString& nickname, const QString& password,
                                   const QString& confirmPassword, bool rememberMe) {
  if (password.length() < static_cast<int>(nyx::kMinAccountPasswordLen)) {
    account_gate_error_ = QStringLiteral("Пароль не короче 8 символов");
    emit accountGateChanged();
    return false;
  }
  if (password != confirmPassword) {
    account_gate_error_ = QStringLiteral("Пароли не совпадают");
    emit accountGateChanged();
    return false;
  }
  std::string err;
  std::string phrase;
  if (!nyx::create_account(nickname.trimmed().toStdString(), password.toStdString(), &phrase,
                           nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  if (rememberMe) nyx::enable_remember_me(nullptr);
  pending_recovery_phrase_ = QString::fromStdString(phrase);
  finishAccountUnlock(false);
  emit accountGateChanged();
  return true;
}

bool NodeController::unlockAccount(const QString& accountId, const QString& password,
                                   bool rememberMe) {
  std::string err;
  if (!nyx::unlock_account(accountId.toStdString(), password.toStdString(), rememberMe, nullptr,
                           &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  pending_recovery_phrase_.clear();
  finishAccountUnlock(true);
  return true;
}

bool NodeController::tryUnlockRemembered(const QString& accountId) {
  if (accountId.trimmed().isEmpty()) return false;
  std::string err;
  if (!nyx::try_unlock_remembered(accountId.toStdString(), nullptr, &err)) {
    return false;
  }
  pending_recovery_phrase_.clear();
  finishAccountUnlock(true);
  return true;
}

bool NodeController::resetPasswordWithRecovery(const QString& accountId,
                                               const QString& recoveryPhrase,
                                               const QString& newPassword,
                                               const QString& confirmPassword) {
  if (newPassword.length() < static_cast<int>(nyx::kMinAccountPasswordLen)) {
    account_gate_error_ = QStringLiteral("Пароль не короче 8 символов");
    emit accountGateChanged();
    return false;
  }
  if (newPassword != confirmPassword) {
    account_gate_error_ = QStringLiteral("Пароли не совпадают");
    emit accountGateChanged();
    return false;
  }
  std::string err;
  if (!nyx::reset_password_with_recovery(accountId.toStdString(), recoveryPhrase.toStdString(),
                                         newPassword.toStdString(), &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  account_gate_error_.clear();
  showToast(QStringLiteral("Пароль обновлён — войдите с новым паролем"));
  emit accountGateChanged();
  return true;
}

void NodeController::confirmRecoveryPhraseSaved() {
  if (pending_recovery_phrase_.isEmpty()) return;
  pending_recovery_phrase_.clear();
  finishAccountUnlock(true);
  emit accountGateChanged();
}

void NodeController::copyRecoveryPhrase() {
  if (pending_recovery_phrase_.isEmpty()) return;
  QGuiApplication::clipboard()->setText(pending_recovery_phrase_);
  showToast(QStringLiteral("Recovery-фраза скопирована"));
}

bool NodeController::importLegacyProfile(const QString& password) {
  if (password.length() < static_cast<int>(nyx::kMinAccountPasswordLen)) {
    account_gate_error_ = QStringLiteral("Пароль не короче 8 символов");
    emit accountGateChanged();
    return false;
  }
  std::string err;
  std::string phrase;
  if (!nyx::import_legacy_profile(password.toStdString(), &phrase, nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  legacy_profile_pending_ = false;
  pending_recovery_phrase_ = QString::fromStdString(phrase);
  finishAccountUnlock(false);
  emit accountGateChanged();
  return true;
}

void NodeController::signOut() {
  service_.stop();
  disconnectSession();
  lan_discovery_timer_.stop();
  nyx::lock_session(true);
  service_.clear_account_data();
  resetFilesUiState();
  resetFileBrowse();
  session_unlocked_ = false;
  pending_recovery_phrase_.clear();
  profile_nickname_.clear();
  profile_id_short_.clear();
  account_gate_error_.clear();
  refreshAccountList();
  emit sessionUnlockedChanged();
  emit profileChanged();
  emit chatChanged();
}

void NodeController::updateOnboardingFlag() {
  needs_onboarding_ = false;
}

void NodeController::refreshProfile() {
  const auto profile = service_.profile();
  profile_nickname_ = QString::fromStdString(profile.nickname);
  profile_id_short_ = QString::fromStdString(nyx::short_user_id(profile.user_id()));
  profile_user_id_hex_ =
      QString::fromStdString(nyx::to_hex(profile.user_id().data(), profile.user_id().size()));
  updateOnboardingFlag();
  emit profileChanged();
}

void NodeController::completeOnboarding(const QString& nickname) {
  setNickname(nickname);
}

void NodeController::refreshChatList() {
  chat_list_.refreshFromDisk(profile_id_short_);

  auto rank = [](const QString& state) -> int {
    if (state == QLatin1String("live")) return 3;
    if (state == QLatin1String("connecting")) return 2;
    if (state == QLatin1String("offline") || state == QLatin1String("disconnected")) return 1;
    return 0;
  };
  auto put_state = [&](QHash<QString, QString>& map, const QString& key, const QString& state) {
    if (key.isEmpty()) return;
    const QString norm = normalizeSessionKey(key);
    const auto it = map.constFind(norm);
    if (it == map.cend() || rank(state) >= rank(it.value())) map.insert(norm, state);
  };

  QHash<QString, QString> live_states;
  for (const auto& info : service_.list_sessions()) {
    if (info.kind == nyx_app::SessionKind::DmInbox) continue;
    const QString sid = QString::fromStdString(info.id);
    const QString state = QString::fromUtf8(nyx_app::session_state_name(info.state));
    put_state(live_states, sid, state);
    const QString ref = QString::fromStdString(info.ref_id_hex).trimmed().toLower();
    if (ref.isEmpty()) continue;
    if (info.kind == nyx_app::SessionKind::GroupHub ||
        info.kind == nyx_app::SessionKind::GroupMember ||
        sid.startsWith(QStringLiteral("group:"))) {
      put_state(live_states, QStringLiteral("group:") + ref, state);
    }
    if (info.kind == nyx_app::SessionKind::Direct || sid.startsWith(QStringLiteral("dm:"))) {
      if (!sid.startsWith(QStringLiteral("dm:pending:")) &&
          !sid.startsWith(QStringLiteral("dm:incoming:"))) {
        put_state(live_states, QStringLiteral("dm:") + ref, state);
      }
    }
  }

  for (int i = 0; i < chat_list_.rowCount(); ++i) {
    const QModelIndex idx = chat_list_.index(i, 0);
    const QString key = chat_list_.data(idx, ChatListModel::KeyRole).toString();
    if (key.isEmpty()) continue;
    const QString norm = normalizeSessionKey(key);
    if (live_states.contains(norm)) {
      chat_list_.setSessionState(key, live_states.value(norm));
    } else {
      chat_list_.setSessionState(key, QStringLiteral("offline"));
    }
  }
}

void NodeController::refreshGroupList() {
  group_list_.clear();
  nyx::Profile profile;
  if (!nyx::active_profile(profile)) {
    emit groupListChanged();
    return;
  }
  const auto groups = service_.list_groups();
  for (auto g : groups) {
    const std::string owner_nick =
        (g.owner_id == profile.user_id()) ? profile.nickname : std::string{};
    nyx::GroupStore::ensure_roster(g, owner_nick);
    QVariantMap m;
    const QString gid = QString::fromStdString(nyx::GroupStore::group_id_hex(g.id));
    m.insert(QStringLiteral("groupId"), gid);
    m.insert(QStringLiteral("name"), QString::fromStdString(g.name));
    m.insert(QStringLiteral("invite"),
             QString::fromStdString(nyx::GroupStore::invite_hex(g.invite_token)));
    bool is_owner = g.owner_id == profile.user_id();
    if (!is_owner) {
      for (const auto& mem : g.members) {
        if (mem.role == nyx::GroupRole::Owner && mem.user_id == profile.user_id()) {
          is_owner = true;
          break;
        }
      }
    }
    m.insert(QStringLiteral("isOwner"), is_owner);
    m.insert(QStringLiteral("roleLabel"),
             is_owner ? QStringLiteral("Создатель") : QStringLiteral("Участник"));
    m.insert(QStringLiteral("memberCount"), static_cast<int>(g.members.size()));
    m.insert(QStringLiteral("hubOnline"),
             service_.is_group_hub_running(gid.toStdString()));

    QVariantList members;
    for (const auto& member : g.members) {
      QVariantMap mm;
      const QString uid =
          QString::fromStdString(nyx::to_hex(member.user_id.data(), member.user_id.size()));
      mm.insert(QStringLiteral("userId"), uid);
      mm.insert(QStringLiteral("nickname"), QString::fromStdString(member.nickname));
      mm.insert(QStringLiteral("isOwner"), member.role == nyx::GroupRole::Owner);
      mm.insert(QStringLiteral("idShort"),
                QString::fromStdString(nyx::short_user_id(member.user_id)));
      members.append(mm);
    }
    m.insert(QStringLiteral("members"), members);
    group_list_.append(m);
  }
  emit groupListChanged();
  emit chatChanged();
}

void NodeController::loadStoredHistory(int kind, const QString& refId,
                                      const QString& convKey) {
  messages_.clear();
  const auto profile = service_.profile();
  std::string path;

  const std::string key = convKey.toStdString();
  if (key.rfind("chat:", 0) == 0) {
    path = nyx::data_dir() + "/chats/" + key.substr(5) + ".jsonl";
  } else if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
    nyx::GroupId gid{};
    if (!nyx::GroupStore::group_id_from_hex(refId.toStdString(), gid)) return;
    path = nyx::MessageStore::path_for_group(gid);
  } else {
    nyx::UserId peer{};
    if (!parse_user_id_hex(refId, peer)) return;
    path = nyx::MessageStore::path_for_chat(nyx::dm_chat_id(profile.user_id(), peer));
  }

  nyx::MessageStore store(path);
  for (const auto& stored : store.recent(100)) {
    messages_.appendMessage(QString::fromStdString(stored.author),
                            QString::fromStdString(stored.text), stored.outgoing,
                            stored.timestamp_ms);
  }
}

void NodeController::openConversation(const QString& key, int kind, const QString& refId,
                                      const QString& title, const QString& lastSeen) {
  const bool live = service_.is_session_live(key.toStdString());
  const bool is_group = kind == static_cast<int>(nyx::ConversationKind::Group);

  // Нужен active_chat_ref_id_ для activeFieldIsOwner() до проверки owner.
  active_chat_kind_ = kind;
  active_chat_ref_id_ = refId;
  const bool owner_field = is_group && activeFieldIsOwner();

  if (!live && !owner_field) {
    chat_list_.setSessionState(key, QStringLiteral("offline"));
    chat_list_.setSelectedKey({});
    active_chat_key_.clear();
    peer_title_.clear();
    peer_connection_label_.clear();
    peer_status_text_.clear();
    in_chat_ = false;
    messages_.clear();
    if (is_group) {
      showToast(QStringLiteral("Владелец поля не в сети"), false);
    } else {
      Q_UNUSED(lastSeen);
      showToast(QStringLiteral("Собеседник не в сети"), false);
    }
    emit chatChanged();
    emit filesChanged();
    return;
  }

  active_chat_key_ = key;
  peer_title_ = title;
  peer_connection_label_.clear();
  service_.set_active_session(key.toStdString());
  chat_list_.setSelectedKey(key);

  in_chat_ = live;
  if (live) {
    peer_status_text_ = is_group ? QStringLiteral("в поле") : QStringLiteral("в сети");
  } else {
    peer_status_text_ = QStringLiteral("hub не запущен");
  }

  loadStoredHistory(kind, refId, key);
  chat_list_.clearUnread(key);
  emit chatChanged();
  emit filesChanged();

  if (live) return;

  // Владелец поля: поднять hub и сразу дать писать после Live.
  chat_list_.setSessionState(key, QStringLiteral("connecting"));
  showToast(QStringLiteral("Запуск hub поля…"));
  QTimer::singleShot(0, this, [this, key]() {
    if (!service_.ensure_session(key.toStdString())) {
      peer_status_text_ = QStringLiteral("hub не запущен");
      in_chat_ = false;
      chat_list_.setSessionState(key, QStringLiteral("offline"));
      showToast(QStringLiteral("Не удалось запустить hub поля"), true);
      emit chatChanged();
      refreshChatList();
    }
    emit busyChanged();
    emit sessionsChanged();
  });
}

void NodeController::searchMessages(const QString& query) {
  messages_.setFilter(query);
}

void NodeController::showWindow() { emit showMainWindow(); }

void NodeController::hideToTray() {
  if (tray_icon_) tray_icon_->show();
  emit requestCloseToTray();
}

void NodeController::setStatus(const QString& text) {
  status_text_ = text;
  emit statusTextChanged();
  emit logLine(QDateTime::currentDateTime().toString("hh:mm:ss") + "  " + text);
}

void NodeController::showToast(const QString& text, bool isError) {
  if (text.isEmpty()) return;
  toast_is_error_ = isError;
  // Полный текст — перенос в ToastHost; без обрезки до короткого «кусочка».
  toast_ = text;
  emit toastChanged();
}

QString NodeController::normalizeInviteHex(const QString& hex) const {
  QString t = hex.trimmed();
  if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) t = t.mid(2);
  t.remove(QChar(' '));
  t.remove(QChar('\n'));
  t.remove(QChar('\r'));
  t.remove(QChar('\t'));
  return t.toLower();
}

void NodeController::enterChat(const QString& peerName, const QString& connectionLabel,
                               int kind, const QString& refId) {
  in_chat_ = true;
  peer_title_ = peerName;
  peer_connection_label_ = connectionLabel;
  active_chat_kind_ = kind;
  active_chat_ref_id_ = refId;
  if (kind == static_cast<int>(nyx::ConversationKind::Group) && !refId.isEmpty()) {
    peer_status_text_ = QStringLiteral("в поле");
    active_chat_key_ = QStringLiteral("group:") + refId;
    const QString gid = refId.trimmed().toLower();
    if (file_scope_group_id_ != gid) {
      file_scope_group_id_ = gid;
      syncFileScopeLabel();
    }
  } else {
    peer_status_text_ = QStringLiteral("в сети");
    if (!refId.isEmpty()) active_chat_key_ = QStringLiteral("dm:") + refId;
  }
  emit chatChanged();
  emit busyChanged();
  emit filesChanged();
}

void NodeController::endLiveSession() {
  const bool was_in = in_chat_;
  in_chat_ = false;
  peer_connection_label_.clear();
  if (active_chat_kind_ == static_cast<int>(nyx::ConversationKind::Group)) {
    peer_status_text_ = activeFieldIsOwner()
                            ? QStringLiteral("hub остановлен")
                            : QStringLiteral("ожидание hub");
  } else if (was_in || peer_status_text_ == QStringLiteral("в сети")) {
    peer_status_text_ = QStringLiteral("история");
  }
  file_progress_visible_ = false;
  file_progress_percent_ = 0;
  file_progress_label_.clear();
  emit fileProgressChanged();
  emit chatChanged();
  emit busyChanged();
  refreshChatList();
  refreshGroupList();
  refreshRemoteFileModel();
  emit filesChanged();
  emit sessionsChanged();
}

void NodeController::leaveChat() {
  in_chat_ = false;
  peer_title_.clear();
  peer_connection_label_.clear();
  peer_status_text_.clear();
  active_chat_key_.clear();
  active_chat_ref_id_.clear();
  active_chat_kind_ = 0;
  messages_.clear();
  file_progress_visible_ = false;
  file_progress_percent_ = 0;
  file_progress_label_.clear();
  emit fileProgressChanged();
  emit chatChanged();
  emit busyChanged();
  refreshChatList();
}

void NodeController::showGroupInView(const QString& groupIdHex) {
  refreshGroupList();
  active_chat_kind_ = static_cast<int>(nyx::ConversationKind::Group);
  active_chat_ref_id_ = groupIdHex;
  active_chat_key_ = QStringLiteral("group:") + groupIdHex;
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() == groupIdHex) {
      peer_title_ = m.value(QStringLiteral("name")).toString();
      break;
    }
  }
  peer_connection_label_.clear();
  peer_status_text_ = QStringLiteral("поле");
  loadStoredHistory(active_chat_kind_, groupIdHex, active_chat_key_);
  emit chatChanged();
}

void NodeController::startListen() {
  if (!service_.start_listen(true)) {
    setStatus(QStringLiteral("Не удалось начать прослушивание"));
    return;
  }
  showToast(QStringLiteral("Ожидание подключения — token появится ниже"));
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
}

void NodeController::refreshLanPeers() {
  service_.scan_lan_peers(3500);
}

void NodeController::tickLanDiscovery() {
  refreshLanPeers();
}

void NodeController::connectToken(const QString& tokenHex) {
  if (!service_.start_connect_token(tokenHex.trimmed().toStdString())) {
    setStatus(QStringLiteral("Не удалось подключиться"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
}

void NodeController::connectPeer(const QString& host, int port) {
  if (!service_.start_connect_peer(host.toStdString(), static_cast<uint16_t>(port))) {
    setStatus(QStringLiteral("Не удалось подключиться"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
}

void NodeController::disconnectSession() {
  disconnectChat(active_chat_key_);
}

void NodeController::disconnectChat(const QString& key) {
  const QString sid = key.isEmpty() ? active_chat_key_ : key;
  if (sid.isEmpty()) {
    service_.stop_session();
  } else {
    service_.stop_session(sid.toStdString());
    service_.mark_session_disconnected(sid.toStdString());
  }
  if (sid == active_chat_key_ || active_chat_key_.isEmpty()) {
    endLiveSession();
  }
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
  refreshChatList();
}

void NodeController::sendMessage(const QString& text) {
  if (text.trimmed().isEmpty()) return;
  if (!canSendMessage()) {
    showToast(QStringLiteral("Нет связи с собеседником — сообщение не отправлено"), true);
    return;
  }
  if (!service_.send_message(text.toStdString(), active_chat_key_.toStdString())) {
    showToast(QStringLiteral("Не удалось отправить сообщение"), true);
  }
}

void NodeController::createGroup(const QString& name) {
  service_.create_group(name.trimmed().toStdString());
}

void NodeController::deleteGroup(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (gid.isEmpty()) {
    showToast(QStringLiteral("Поле не выбрано"));
    return;
  }

  if (active_chat_ref_id_.trimmed().toLower() == gid) {
    service_.stop();
    endLiveSession();
    peer_title_.clear();
    active_chat_key_.clear();
    active_chat_ref_id_.clear();
    active_chat_kind_ = 0;
    messages_.clear();
    emit chatChanged();
  }

  if (!service_.delete_group(gid.toStdString())) {
    showToast(QStringLiteral("Не удалось удалить поле"));
    return;
  }
  refreshGroupList();
  refreshChatList();
  showToast(QStringLiteral("Поле удалено"));
}

void NodeController::removeFieldMember(const QString& groupIdHex, const QString& userIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  const QString uid = userIdHex.trimmed().toLower();
  if (gid.size() != 64 || uid.size() != 64) {
    showToast(QStringLiteral("Неверные id поля или участника"));
    return;
  }
  if (uid == profile_user_id_hex_.trimmed().toLower()) {
    showToast(QStringLiteral("Нельзя исключить себя"));
    return;
  }
  if (!service_.remove_group_member(gid.toStdString(), uid.toStdString())) {
    showToast(QStringLiteral("Не удалось исключить участника"));
    return;
  }
  refreshGroupList();
  showToast(QStringLiteral("Участник исключён"));
}

void NodeController::startFieldHub(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (gid.size() != 64) {
    showToast(QStringLiteral("Неверный id поля"));
    return;
  }
  showGroupInView(gid);
  setGroupsDialogOpen(false);
  peer_status_text_ = QStringLiteral("запуск…");
  emit chatChanged();
  showToast(QStringLiteral("Запуск поля…"));
  service_.start_group_hub(gid.toStdString());
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
}

void NodeController::joinField(const QString& inviteHex) {
  const QString normalized = normalizeInviteHex(inviteHex);
  if (normalized.size() != 64) {
    const QString err = QStringLiteral("Invite поля: нужно 64 hex-символа");
    setStatus(err);
    showToast(err);
    return;
  }
  setGroupsDialogOpen(false);
  pending_field_join_notify_ = true;

  // Intent сразу: иначе после появления владельца список так и останется offline.
  nyx::InviteToken token{};
  if (nyx::GroupStore::invite_from_hex(normalized.toStdString(), token)) {
    nyx::GroupStore store;
    store.load();
    for (const auto& gr : store.all()) {
      if (gr.invite_token != token) continue;
      const std::string key =
          nyx_app::make_group_session_id(nyx::GroupStore::group_id_hex(gr.id));
      nyx::SessionIntent intent;
      intent.key = key;
      intent.kind = nyx::SessionIntentKind::GroupJoin;
      intent.ref_id_hex = nyx::GroupStore::group_id_hex(gr.id);
      intent.invite_hex = normalized.toStdString();
      intent.enabled = true;
      service_.enable_session_intent(std::move(intent));
      chat_list_.setSessionState(QString::fromStdString(key), QStringLiteral("connecting"));
      break;
    }
  }

  showToast(QStringLiteral("Подключение к полю…"));
  service_.start_group_join(normalized.toStdString());
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
  refreshChatList();
}

void NodeController::connectActiveField() {
  if (active_chat_kind_ != static_cast<int>(nyx::ConversationKind::Group)) {
    showToast(QStringLiteral("Выберите поле в списке чатов"));
    return;
  }
  if (active_chat_ref_id_.isEmpty()) {
    showToast(QStringLiteral("Поле не выбрано"));
    return;
  }

  nyx::Profile profile;
  if (!nyx::active_profile(profile)) {
    showToast(QStringLiteral("Войдите в аккаунт"));
    return;
  }

  const QString gid = active_chat_ref_id_.trimmed().toLower();
  if (gid.size() != 64) {
    showToast(QStringLiteral("Неверный id поля"));
    return;
  }

  refreshGroupList();

  bool is_owner = false;
  QString invite;
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid) continue;
    is_owner = m.value(QStringLiteral("isOwner")).toBool();
    invite = m.value(QStringLiteral("invite")).toString();
    break;
  }

  if (invite.isEmpty()) {
    nyx::GroupStore store;
    store.load();
    nyx::GroupId group_id{};
    if (!nyx::GroupStore::group_id_from_hex(gid.toStdString(), group_id)) {
      showToast(QStringLiteral("Неверный id поля"));
      return;
    }
    const auto group = store.find(group_id);
    if (!group) {
      showToast(QStringLiteral("Поле не найдено — откройте «Поля»"));
      return;
    }
    is_owner = group->owner_id == profile.user_id();
    invite = QString::fromStdString(nyx::GroupStore::invite_hex(group->invite_token));
  }

  if (is_owner) {
    startFieldHub(gid);
  } else {
    joinField(invite);
  }
}

void NodeController::copyToClipboard(const QString& text) {
  if (text.isEmpty()) return;
  QGuiApplication::clipboard()->setText(text);
  toast_ = QStringLiteral("Скопировано");
  emit toastChanged();
}

void NodeController::copyInviteToken() { copyToClipboard(invite_token_); }

void NodeController::copyDmInboxToken() {
  copyToClipboard(QString::fromStdString(service_.dm_inbox_token_hex()));
}

void NodeController::copyLastGroupInvite() { copyToClipboard(last_group_invite_); }

void NodeController::clearToast() {
  if (toast_.isEmpty()) return;
  toast_.clear();
  toast_is_error_ = false;
  emit toastChanged();
}

void NodeController::setWindowActive(bool active) {
  if (window_active_ == active) return;
  window_active_ = active;
  emit windowActiveChanged();
}
