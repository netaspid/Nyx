#include "node_controller.hpp"

#include "nyx/account_store.hpp"
#include "nyx/chat_id.hpp"
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
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVariantMap>

#include <cstring>

namespace {

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
  emit accountGateChanged();

  if (session_unlocked_) {
    beginMainSession();
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
  refreshFileAccessLists();

  service_.load_network_config();
  syncNetworkSettingsFromService();
  refreshProfile();
  refreshChatList();
  refreshGroupList();
  lan_discovery_timer_.setInterval(5000);
  connect(&lan_discovery_timer_, &QTimer::timeout, this, &NodeController::tickLanDiscovery);
  lan_discovery_timer_.start();
  QTimer::singleShot(800, this, &NodeController::tickLanDiscovery);
  QTimer::singleShot(2000, this, &NodeController::maybeAutoStartOwnedHub);
}

NodeController::~NodeController() {
  lan_discovery_timer_.stop();
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

void NodeController::maybeAutoStartOwnedHub() {
  if (!auto_start_owned_hub_ || service_.busy() || in_chat_) return;

  nyx::Profile profile;
  if (!nyx::active_profile(profile)) return;

  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (!m.value(QStringLiteral("isOwner")).toBool()) continue;
    const QString gid = m.value(QStringLiteral("groupId")).toString();
    if (gid.isEmpty()) continue;
    startFieldHub(gid);
    break;
  }
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
    refreshFileLists();
    refreshFileAccessLists();
    if (fileExchangeReady()) refreshRemoteFileList();
  }
  emit mainViewModeChanged();
}

uint32_t NodeController::currentFilePermissions() const {
  return service_.my_file_permissions(file_scope_group_id_.toStdString(), {});
}

bool NodeController::canFileList() const {
  if (file_scope_group_id_.isEmpty()) return true;
  return hasFilePermission(static_cast<int>(nyx::FilePermission::List));
}

bool NodeController::hasFilePermission(int permissionBit) const {
  return nyx::FileAccessStore::has_permission(currentFilePermissions(),
                                              static_cast<nyx::FilePermission>(permissionBit));
}

bool NodeController::canManageFileRoles() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::ManageRoles));
}

bool NodeController::canFileUpload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Upload));
}

bool NodeController::canFileDownload() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::Download));
}

bool NodeController::canFileOpenRemote() const {
  return hasFilePermission(static_cast<int>(nyx::FilePermission::OpenRemote));
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
  file_member_access_.clear();
  if (file_scope_group_id_.isEmpty()) {
    emit fileAccessChanged();
    return;
  }

  const auto policy = service_.file_access_policy(file_scope_group_id_.toStdString());
  for (const auto& role : policy.roles) {
    QVariantMap rm;
    rm.insert(QStringLiteral("roleId"), QString::fromStdString(role.id));
    rm.insert(QStringLiteral("name"), QString::fromStdString(role.name));
    rm.insert(QStringLiteral("permissions"), static_cast<int>(role.permissions));
    rm.insert(QStringLiteral("builtin"), role.builtin);
    file_role_list_.append(rm);
  }

  const QString scope = file_scope_group_id_.trimmed().toLower();
  for (const QVariant& gv : group_list_) {
    const QVariantMap gm = gv.toMap();
    if (gm.value(QStringLiteral("groupId")).toString() != scope) continue;
    const QVariantList members = gm.value(QStringLiteral("members")).toList();
    for (const QVariant& mv : members) {
      const QVariantMap mm = mv.toMap();
      const QString uid = mm.value(QStringLiteral("userId")).toString();
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
      row.insert(QStringLiteral("nickname"), mm.value(QStringLiteral("nickname")));
      row.insert(QStringLiteral("idShort"), mm.value(QStringLiteral("idShort")));
      row.insert(QStringLiteral("isOwner"), mm.value(QStringLiteral("isOwner")));
      row.insert(QStringLiteral("roleId"), role_id);
      row.insert(QStringLiteral("roleName"), role_name);
      file_member_access_.append(row);
    }
    break;
  }

  emit fileAccessChanged();
}

void NodeController::refreshFileAccess() { refreshFileAccessLists(); }

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
    showToast(QStringLiteral("Нет права управлять ролями"));
    return;
  }
  for (const QVariant& rv : file_role_list_) {
    const QVariantMap rm = rv.toMap();
    if (rm.value(QStringLiteral("roleId")).toString() != roleId) continue;
    if (rm.value(QStringLiteral("builtin")).toBool() &&
        roleId == QString::fromStdString(nyx::FileAccessStore::role_id_owner())) {
      showToast(QStringLiteral("Роль владельца нельзя менять"));
      return;
    }
    int perms = rm.value(QStringLiteral("permissions")).toInt();
    perms ^= permissionBit;
    updateFileRole(roleId, rm.value(QStringLiteral("name")).toString(), perms);
    return;
  }
}

void NodeController::openRemoteFile(const QString& hashHex) {
  if (!canFileOpenRemote()) {
    showToast(QStringLiteral("Нет права открывать файлы по сети"));
    return;
  }
  if (!service_.download_file(hashHex.trimmed().toStdString())) {
    showToast(QStringLiteral("Не удалось запросить файл"));
    return;
  }
  showToast(QStringLiteral("Скачивание для локального открытия…"));
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
  refreshFileLists();
  refreshFileAccessLists();
  emit filesChanged();
  emit fileAccessChanged();
}

void NodeController::openGroupsDialog() {
  refreshGroupList();
  setGroupsDialogOpen(true);
}

bool NodeController::fileExchangeReady() const { return service_.can_request_remote_files(); }

QString NodeController::fileExchangeHint() const {
  return QString::fromStdString(service_.file_exchange_hint());
}

QVariantList NodeController::entriesToVariant(const std::vector<nyx::FileEntry>& entries,
                                              bool remote) const {
  QVariantList list;
  for (const auto& e : entries) {
    QVariantMap m;
    m.insert(QStringLiteral("name"), QString::fromStdString(e.leaf_name()));
    m.insert(QStringLiteral("navPath"), QString::fromStdString(e.relative_path));
    m.insert(QStringLiteral("rootPath"), QString::fromStdString(e.root_path));
    m.insert(QStringLiteral("hash"), QString::fromStdString(nyx::hash_hex(e.hash)));
    m.insert(QStringLiteral("size"), static_cast<qulonglong>(e.size));
    m.insert(QStringLiteral("mime"), QString::fromStdString(e.mime));
    m.insert(QStringLiteral("isRemote"), remote);
    m.insert(QStringLiteral("isDirectory"), e.is_directory());
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
  local_files_.setEntries({});
  remote_files_.setEntries({});
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
  refreshRemoteFileModel({});
  service_.save_files_selected_root(canonical.toStdString());
  emit filesChanged();
}

void NodeController::browseIntoFolder(const QString& navPath) {
  if (navPath.trimmed().isEmpty()) return;
  QString rel = navPath.trimmed();
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  file_browse_path_ = rel;
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  refreshRemoteFileModel({});
  emit filesChanged();
}

void NodeController::browseUp() {
  if (file_browse_path_.isEmpty()) return;
  QString rel = file_browse_path_;
  rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
  const int slash = rel.lastIndexOf(QLatin1Char('/'));
  file_browse_path_ = slash < 0 ? QString() : rel.left(slash);
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  refreshRemoteFileModel({});
  emit filesChanged();
}

void NodeController::browseToCrumb(int index) {
  if (index < 0 || index >= file_browse_crumbs_.size()) return;
  file_browse_path_ = file_browse_crumbs_.at(index).toMap().value(QStringLiteral("path")).toString();
  syncFileBrowseCrumbs();
  refreshLocalFileModel();
  refreshRemoteFileModel({});
  emit filesChanged();
}

void NodeController::addDroppedUrls(const QVariantList& urls) {
  if (urls.isEmpty()) return;
  if (!canAddShareFolder()) {
    showToast(QStringLiteral("Нет права добавлять папки в эту область"));
    return;
  }
  int added = 0;
  for (const QVariant& u : urls) {
    QString p = u.toString().trimmed();
    if (p.startsWith(QStringLiteral("file:///"))) p = QUrl(p).toLocalFile();
    if (p.isEmpty()) continue;
    QFileInfo info(p);
    if (info.isDir()) {
      if (service_.index_folder(p.toStdString(), file_scope_group_id_.toStdString())) ++added;
    } else if (info.isFile()) {
      const QString dir = info.absolutePath();
      if (service_.index_folder(dir.toStdString(), file_scope_group_id_.toStdString())) ++added;
    }
  }
  refreshFileLists();
  if (added > 0) {
    showToast(QStringLiteral("Добавлено папок: %1").arg(added));
  } else {
    showToast(QStringLiteral("Не удалось добавить из перетаскивания"));
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
    local_files_.setEntries({});
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
  local_files_.setEntries(entriesToVariant(entries, false));
}

void NodeController::refreshRemoteFileModel(const std::vector<nyx::FileEntry>& entries) {
  if (file_selected_share_root_.isEmpty()) {
    remote_files_.setEntries({});
    return;
  }
  std::string root_path = file_selected_share_root_.toStdString();
  for (const auto& r : service_.all_share_roots()) {
    if (shareRootPathsEqual(file_selected_share_root_, QString::fromStdString(r.path))) {
      root_path = r.path;
      break;
    }
  }
  const auto& all = entries.empty() ? service_.remote_files() : entries;
  const auto level = nyx::FileIndex::listing_level(
      all, root_path, file_browse_path_.toStdString());
  remote_files_.setEntries(entriesToVariant(level, true));
}

void NodeController::refreshFileLists() {
  refreshFileShareRoots();
  refreshLocalFileModel();
  refreshRemoteFileModel({});
  emit filesChanged();
}

QString NodeController::pickFolder() {
  const QString dir =
      QFileDialog::getExistingDirectory(nullptr, QStringLiteral("Выберите папку для индекса"),
                                        QDir::homePath());
  return dir;
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
  if (!service_.index_folder(p.toStdString(), file_scope_group_id_.toStdString())) {
    showToast(status_text_.isEmpty() ? QStringLiteral("Не удалось проиндексировать папку")
                                      : status_text_);
    return;
  }
  refreshFileLists();
  const int count = service_.file_count_in_root(p.toStdString());
  if (count == 0) {
    showToast(QStringLiteral("Папка добавлена (0 файлов). Положите файлы в папку и нажмите «Переиндексировать»."));
  } else {
    showToast(QStringLiteral("Папка проиндексирована: %1 файлов").arg(count));
  }
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
  refreshFileLists();
  showToast(QStringLiteral("Папка убрана из индекса"));
}

void NodeController::rescanIndexedFolder(const QString& path) {
  if (path.trimmed().isEmpty()) return;
  if (!service_.rescan_share_root(path.toStdString(), file_scope_group_id_.toStdString())) {
    showToast(status_text_.isEmpty() ? QStringLiteral("Не удалось переиндексировать")
                                      : status_text_);
    return;
  }
  refreshFileLists();
  showToast(status_text_.isEmpty() ? QStringLiteral("Переиндексировано") : status_text_);
}

void NodeController::refreshRemoteFileList() {
  if (!service_.request_remote_files()) {
    showToast(fileExchangeHint().isEmpty()
                  ? QStringLiteral("Не удалось запросить файлы")
                  : fileExchangeHint());
    return;
  }
}

void NodeController::downloadFile(const QString& hashHex) {
  if (!canFileDownload()) {
    showToast(QStringLiteral("Нет права скачивать файлы"));
    return;
  }
  if (!service_.download_file(hashHex.trimmed().toStdString())) {
    showToast(QStringLiteral("Не удалось скачать файл"));
  }
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
          if (lower.contains(QStringLiteral("failed")) ||
              lower.contains(QStringLiteral("не удалось")) ||
              lower.contains(QStringLiteral("неверн")) ||
              lower.contains(QStringLiteral("lookup")) ||
              lower.contains(QStringLiteral("register")) ||
              lower.contains(QStringLiteral("timeout")) ||
              lower.contains(QStringLiteral("handshake")) ||
              lower.contains(QStringLiteral("поле")) ||
              lower.contains(QStringLiteral("hub")) ||
              lower.contains(QStringLiteral("invite")) ||
              lower.contains(QStringLiteral("rendezvous")) ||
              lower.contains(QStringLiteral("bind"))) {
            showToast(q);
            if (!in_chat_ &&
                active_chat_kind_ == static_cast<int>(nyx::ConversationKind::Group)) {
              peer_status_text_ = QStringLiteral("поле");
              emit chatChanged();
            }
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
          messages_.appendMessage(QString::fromStdString(msg.author),
                                  QString::fromStdString(msg.text), msg.outgoing,
                                  msg.timestamp_ms);
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

  service_.set_on_chat_ready([this](const std::string& peer_title, const std::string& conn_label,
                                    nyx::ConversationKind kind,
                                    const std::string& ref_id) {
    QMetaObject::invokeMethod(
        this,
        [this, peer_title, conn_label, kind, ref_id]() {
          active_chat_kind_ = static_cast<int>(kind);
          active_chat_ref_id_ = QString::fromStdString(ref_id);
          enterChat(QString::fromStdString(peer_title), QString::fromStdString(conn_label),
                    static_cast<int>(kind), QString::fromStdString(ref_id));
          if (kind == nyx::ConversationKind::Group) {
            loadStoredHistory(static_cast<int>(kind), QString::fromStdString(ref_id),
                              active_chat_key_);
            showToast(QStringLiteral("В поле «") + QString::fromStdString(peer_title) +
                      QStringLiteral("»"));
          }
          refreshChatList();
          refreshGroupList();
        },
        Qt::QueuedConnection);
  });

  service_.set_on_session_ended([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          const bool resume = resume_hub_after_p2p_;
          const QString hub_id = resume_hub_id_;
          resume_hub_after_p2p_ = false;
          resume_hub_id_.clear();
          endLiveSession();
          refreshGroupList();
          if (resume && !hub_id.isEmpty()) {
            QTimer::singleShot(500, this, [this, hub_id]() {
              startFieldHub(hub_id);
              showToast(QStringLiteral("Hub поля снова в сети"));
            });
          }
        },
        Qt::QueuedConnection);
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
              file_index_progress_visible_ = !finished;
              if (finished) {
                file_index_progress_percent_ = 100;
                file_index_progress_label_ =
                    QStringLiteral("Готово: %1 файлов").arg(files_scanned);
                refreshFileLists();
                QTimer::singleShot(1200, this, [this]() {
                  file_index_progress_visible_ = false;
                  emit fileIndexProgressChanged();
                });
              } else {
                file_index_progress_percent_ =
                    qMin(95, qMax(5, files_scanned % 100));
                const QString name = QString::fromStdString(path);
                file_index_progress_label_ = name.isEmpty()
                                                 ? QStringLiteral("Сканирование…")
                                                 : name;
              }
              emit fileIndexProgressChanged();
            },
            Qt::QueuedConnection);
      });

  service_.set_on_remote_files([this](const std::vector<nyx::FileEntry>& entries) {
    QMetaObject::invokeMethod(
        this,
        [this, entries]() {
          refreshRemoteFileModel(entries);
          showToast(QStringLiteral("Список файлов собеседника обновлён"));
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
    account_list_.append(m);
  }
  emit accountGateChanged();
}

bool NodeController::createAccount(const QString& nickname, const QString& password,
                                   const QString& confirmPassword) {
  if (password.length() < 8) {
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
  if (!nyx::create_account(nickname.trimmed().toStdString(), password.toStdString(), nullptr,
                           &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  account_gate_error_.clear();
  session_unlocked_ = true;
  emit sessionUnlockedChanged();
  refreshAccountList();
  beginMainSession();
  return true;
}

bool NodeController::unlockAccount(const QString& accountId, const QString& password) {
  std::string err;
  if (!nyx::unlock_account(accountId.toStdString(), password.toStdString(), nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  account_gate_error_.clear();
  session_unlocked_ = true;
  emit sessionUnlockedChanged();
  refreshAccountList();
  beginMainSession();
  return true;
}

bool NodeController::importLegacyProfile(const QString& password) {
  if (password.length() < 8) {
    account_gate_error_ = QStringLiteral("Пароль не короче 8 символов");
    emit accountGateChanged();
    return false;
  }
  std::string err;
  if (!nyx::import_legacy_profile(password.toStdString(), nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  legacy_profile_pending_ = false;
  account_gate_error_.clear();
  session_unlocked_ = true;
  emit sessionUnlockedChanged();
  refreshAccountList();
  beginMainSession();
  return true;
}

void NodeController::signOut() {
  service_.stop();
  disconnectSession();
  lan_discovery_timer_.stop();
  nyx::lock_session();
  service_.clear_account_data();
  resetFilesUiState();
  resetFileBrowse();
  session_unlocked_ = false;
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
}

void NodeController::refreshGroupList() {
  group_list_.clear();
  nyx::Profile profile;
  if (!nyx::active_profile(profile)) {
    emit groupListChanged();
    return;
  }
  const auto groups = service_.list_groups();
  const QString running_hub =
      QString::fromStdString(service_.running_group_hub_id_hex()).toLower();
  for (const auto& g : groups) {
    QVariantMap m;
    const QString gid = QString::fromStdString(nyx::GroupStore::group_id_hex(g.id));
    m.insert(QStringLiteral("groupId"), gid);
    m.insert(QStringLiteral("name"), QString::fromStdString(g.name));
    m.insert(QStringLiteral("invite"),
             QString::fromStdString(nyx::GroupStore::invite_hex(g.invite_token)));
    const bool is_owner = g.owner_id == profile.user_id();
    m.insert(QStringLiteral("isOwner"), is_owner);
    m.insert(QStringLiteral("roleLabel"),
             is_owner ? QStringLiteral("Создатель") : QStringLiteral("Участник"));
    m.insert(QStringLiteral("memberCount"), static_cast<int>(g.members.size()));
    m.insert(QStringLiteral("hubOnline"), !running_hub.isEmpty() && running_hub == gid);

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
  if (main_view_mode_ == 1) refreshFileAccessLists();
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
  active_chat_key_ = key;
  active_chat_kind_ = kind;
  active_chat_ref_id_ = refId;
  peer_title_ = title;
  peer_connection_label_.clear();
  if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
    peer_status_text_ = QStringLiteral("поле");
  } else {
    peer_status_text_ =
        lastSeen.isEmpty() ? QStringLiteral("история") : lastSeen;
  }
  loadStoredHistory(kind, refId, key);
  chat_list_.clearUnread(key);
  emit chatChanged();
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

void NodeController::showToast(const QString& text) {
  if (text.isEmpty()) return;
  toast_ = text.length() > 100 ? text.left(97) + QStringLiteral("…") : text;
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
  if (!in_chat_) return;
  in_chat_ = false;
  peer_connection_label_.clear();
  if (active_chat_kind_ == static_cast<int>(nyx::ConversationKind::Group)) {
    peer_status_text_ = activeFieldIsOwner()
                            ? QStringLiteral("hub остановлен — запустите снова")
                            : QStringLiteral("ожидание hub создателя");
  } else if (peer_status_text_ == QStringLiteral("в сети")) {
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
  refreshRemoteFileModel({});
  emit filesChanged();
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

void NodeController::prepareForConnection() {
  if (service_.mode() == nyx_app::NodeMode::GroupHub && !active_chat_ref_id_.isEmpty()) {
    resume_hub_after_p2p_ = true;
    resume_hub_id_ = active_chat_ref_id_.trimmed().toLower();
    showToast(QStringLiteral("Hub поля остановлен для P2P — после чата перезапустится"));
  } else {
    resume_hub_after_p2p_ = false;
    resume_hub_id_.clear();
  }
  if (service_.busy()) {
    service_.stop();
  }
  endLiveSession();
  invite_token_.clear();
  emit inviteTokenChanged();
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::startListen() {
  prepareForConnection();
  if (!service_.start_listen(true)) {
    setStatus(QStringLiteral("Не удалось начать прослушивание"));
    return;
  }
  showToast(QStringLiteral("Ожидание подключения — token появится ниже"));
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::refreshLanPeers() {
  if (in_chat_) return;
  service_.scan_lan_peers(3500);
}

void NodeController::tickLanDiscovery() {
  refreshLanPeers();
}

void NodeController::connectToken(const QString& tokenHex) {
  prepareForConnection();
  if (!service_.start_connect_token(tokenHex.trimmed().toStdString())) {
    setStatus(QStringLiteral("Не удалось подключиться"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::connectPeer(const QString& host, int port) {
  prepareForConnection();
  if (!service_.start_connect_peer(host.toStdString(), static_cast<uint16_t>(port))) {
    setStatus(QStringLiteral("Не удалось подключиться"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::disconnectSession() {
  resume_hub_after_p2p_ = false;
  resume_hub_id_.clear();
  service_.stop();
  endLiveSession();
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::sendMessage(const QString& text) {
  if (text.trimmed().isEmpty() || !in_chat_) return;
  service_.send_message(text.toStdString());
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
  resume_hub_after_p2p_ = false;
  resume_hub_id_.clear();
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
  peer_status_text_ = QStringLiteral("подключение…");
  emit chatChanged();
  showToast(QStringLiteral("Подключение к полю…"));
  service_.start_group_join(normalized.toStdString());
  emit listeningChanged();
  emit busyChanged();
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

void NodeController::copyLastGroupInvite() { copyToClipboard(last_group_invite_); }

void NodeController::clearToast() {
  if (toast_.isEmpty()) return;
  toast_.clear();
  emit toastChanged();
}

void NodeController::setWindowActive(bool active) {
  if (window_active_ == active) return;
  window_active_ = active;
  emit windowActiveChanged();
}
