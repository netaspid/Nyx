#include "node_controller.hpp"
#include "android_platform.hpp"
#include "document_viewer.hpp"
#include "host_env.hpp"
#include "win_chrome.hpp"

#include "nyx/account_store.hpp"
#include "nyx/avatar_store.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/conversation.hpp"
#include "nyx/file_access.hpp"
#include "nyx/file_hash.hpp"
#include "nyx/file_index.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/message_store.hpp"
#include "nyx/nat.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_crypto.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMimeDatabase>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTemporaryFile>
#include <QThreadPool>
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

QString safeMediaPathPart(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
  value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
  if (value.isEmpty())
    value = QStringLiteral("Чат");
  return value.left(64);
}

QString mediaRelativeDir(const QString& chatKey, const QString&, const QString& mediaKind) {
  QString stable = chatKey.section(QLatin1Char(':'), 1).toLower();
  if (stable.isEmpty())
    stable = chatKey.toLower();
  stable.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
  if (stable.isEmpty())
    stable = QStringLiteral("local");
  const QString label = chatKey.startsWith(QLatin1String("group:")) ? QStringLiteral("Поле")
                                                                    : QStringLiteral("Личный чат");
  const QString conversation = label + QStringLiteral(" (") + stable.left(8) + QLatin1Char(')');
  const QString leaf = mediaKind == QLatin1String("circle") ? QStringLiteral("Видеокружки")
                                                            : QStringLiteral("Голосовые сообщения");
  return QStringLiteral("Медиа/") + conversation + QLatin1Char('/') + leaf;
}

} // namespace

#include <cmath>
#include <cstring>
#include <limits>
#include <map>

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

QIcon makeTrayIcon() {
  return nyxAppIcon();
}

bool parse_user_id_hex(const QString& hex, nyx::UserId& out) {
  std::vector<uint8_t> bytes;
  if (!nyx::from_hex(hex.toStdString(), bytes) || bytes.size() != out.size())
    return false;
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return true;
}

} // namespace

NodeController::NodeController(QObject* parent) : QObject(parent) {
  connect(&document_viewer_,
          &DocumentViewer::toast,
          this,
          [this](const QString& message, bool isError) { showToast(message, isError); });

#if defined(Q_OS_ANDROID)
  service_.set_call_relay_score(300);
  nyx_android::native_camera_stop();
  nyx_android::voice_capture_stop();
  nyx_android::voice_playback_stop();
  nyx_android::set_voip_audio_mode(false);
  nyx_android::cancel_call_notifications();
#else
  service_.set_call_relay_score(800);
#endif

  call_audio_thread_.setObjectName(QStringLiteral("nyx-call-audio"));
  call_audio_.moveToThread(&call_audio_thread_);
  connect(&call_audio_, &CallAudioIo::startFailed, this, [this]() {
    showToast(QStringLiteral("Микрофон/динамик недоступны — только сигналинг"), true);
  });
  connect(
      &call_audio_, &CallAudioIo::micLevelChanged, this, &NodeController::audioTestLevelChanged);
  connect(&call_audio_, &CallAudioIo::micTestChanged, this, &NodeController::audioTestChanged);
  connect(&call_audio_, &CallAudioIo::localVoiceActiveChanged, this, [this](bool active) {
    const bool small_field =
        service_.call_is_field_room() && service_.call_participants().size() <= 2;
    call_video_.setTransmitEnabled(!service_.call_is_field_room() || small_field || active);
  });
  connect(&call_audio_, &CallAudioIo::dominantSpeakerChanged, this, [this](const QString& peerId) {
    if (!service_.call_is_field_room())
      return;
    if (QDateTime::currentMSecsSinceEpoch() < manual_call_focus_until_ms_)
      return;
    manual_call_focus_.clear();
    call_video_.setFocusedPeerId(peerId);
    if (peerId.isEmpty()) {
      if (call_frames_)
        call_frames_->setPrimaryRemoteKey(QString());
      call_remote_frame_url_.clear();
      emit callRemoteFrameChanged();
    }
    emit callVideoPeersChanged();
  });
  call_audio_thread_.start();

  call_video_thread_.setObjectName(QStringLiteral("nyx-call-video"));
  call_video_.moveToThread(&call_video_thread_);
  call_video_thread_.start();

#if defined(Q_OS_ANDROID)
  connect(
      qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden ||
            state == Qt::ApplicationInactive) {
          if (service_.call_state() == nyx::CallState::Active &&
              service_.call_mode() == nyx::CallMode::AudioVideo && service_.call_camera_on()) {
            resume_call_camera_ = true;
            suspended_call_id_ = QString::fromStdString(service_.call_id_hex());
            call_video_.setCameraEnabled(false);
          }
          return;
        }
        if (state != Qt::ApplicationActive || !resume_call_camera_)
          return;
        const QString current = QString::fromStdString(service_.call_id_hex());
        const bool restore = service_.call_state() == nyx::CallState::Active &&
                             current == suspended_call_id_ && service_.call_camera_on();
        resume_call_camera_ = false;
        suspended_call_id_.clear();
        if (restore) {
          QTimer::singleShot(150, this, [this]() {
            if (service_.call_state() != nyx::CallState::Active || !service_.call_camera_on()) {
              return;
            }
            call_video_.setCameraEnabled(true);
            call_video_.start();
            emit callChanged();
          });
        }
      });
#endif

  connect(&call_video_, &CallVideoIo::cameraOpenFailed, this, [this]() {
    service_.set_call_camera_on(false);
    showToast(QStringLiteral("Не удалось открыть камеру"), true);
    emit callChanged();
  });

#if !defined(Q_OS_ANDROID)
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
    connect(
        tray_icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
          if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
            showWindow();
        });
    tray_icon_->show();
  }
#endif

  wireCallbacks();
  loadMediaDevicePrefs();

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

  service_.load_network_config();
  syncNetworkSettingsFromService();
  refreshProfile();
  loadProfileMeta();
  refreshChatList();
  refreshGroupList();
  refreshContactList();

  QTimer::singleShot(0, this, [this]() {
    refreshFileLists();
    refreshFileAccessLists();
  });
#if defined(Q_OS_ANDROID)
  nyx_android::request_notification_permission();

  QTimer::singleShot(800, this, []() { nyx_android::start_keepalive_service(); });
  QTimer::singleShot(5000, this, []() { nyx_android::start_keepalive_service(); });
#endif

  lan_discovery_timer_.setInterval(12000);
  connect(&lan_discovery_timer_, &QTimer::timeout, this, &NodeController::tickLanDiscovery);
  lan_discovery_timer_.start();
  QTimer::singleShot(1500, this, &NodeController::tickLanDiscovery);
  QTimer::singleShot(400, this, [this]() {
    service_.ensure_owned_hubs_running();
    refreshChatSessionStates();
    emit sessionsChanged();
  });
  QTimer::singleShot(2500, this, &NodeController::maybeAutoReconnectSessions);

  session_reconnect_timer_.setInterval(20000);
  connect(&session_reconnect_timer_,
          &QTimer::timeout,
          this,
          &NodeController::maybeAutoReconnectSessions);
  session_reconnect_timer_.start();
}

NodeController::~NodeController() {
  lan_discovery_timer_.stop();
  session_reconnect_timer_.stop();
#if defined(Q_OS_ANDROID)
  nyx_android::stop_keepalive_service();
  nyx_android::cancel_call_notifications();
#endif
  if (tray_icon_) {
    tray_icon_->hide();
  }
  if (call_audio_thread_.isRunning()) {
    QMetaObject::invokeMethod(
        &call_audio_,
        [this]() {
          call_audio_.stop();
          call_audio_thread_.quit();
        },
        Qt::QueuedConnection);
    if (!call_audio_thread_.wait(5000))
      call_audio_thread_.wait();
  } else {
    call_audio_.stop();
  }
  if (call_video_thread_.isRunning()) {
    QMetaObject::invokeMethod(
        &call_video_,
        [this]() {
          call_video_.stop();
          call_video_thread_.quit();
        },
        Qt::QueuedConnection);
    if (!call_video_thread_.wait(5000))
      call_video_thread_.wait();
  } else {
    call_video_.stop();
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
    rendezvous_ =
        QString::fromStdString(primary.host) + QLatin1Char(':') + QString::number(primary.port);
  }
  discovery_mode_ = static_cast<int>(service_.network_config().mode);
  auto_start_owned_hub_ = service_.auto_start_owned_hub();
  emit rendezvousChanged();
  emit networkSettingsChanged();
}

void NodeController::setAutoStartOwnedHub(bool enabled) {
  if (auto_start_owned_hub_ == enabled)
    return;
  auto_start_owned_hub_ = enabled;
  service_.set_auto_start_owned_hub(enabled);
  emit networkSettingsChanged();
}

bool NodeController::activeFieldIsOwner() const {
  if (active_chat_kind_ != static_cast<int>(nyx::ConversationKind::Group))
    return false;
  if (active_chat_ref_id_.isEmpty())
    return false;

  const QString gid = active_chat_ref_id_.trimmed().toLower();
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid)
      continue;
    return m.value(QStringLiteral("isOwner")).toBool();
  }

  nyx::Profile profile;
  if (!nyx::active_profile(profile))
    return false;

  nyx::GroupStore store;
  store.load();
  nyx::GroupId group_id {};
  if (!nyx::GroupStore::group_id_from_hex(gid.toStdString(), group_id)) {
    return false;
  }
  const auto group = store.find(group_id);
  if (!group)
    return false;
  return group->owner_id == profile.user_id();
}

void NodeController::maybeAutoReconnectSessions() {

  service_.auto_reconnect_all();
  invite_token_ = QString::fromStdString(service_.dm_inbox_token_hex());
  emit inviteTokenChanged();
  emit listeningChanged();
  emit busyChanged();

  refreshChatSessionStates();
  emit sessionsChanged();
}

QString NodeController::sessionSummary() const {
  const std::size_t n = service_.live_session_count();
  if (n == 0)
    return QStringLiteral("Нет активных сессий");
  if (n == 1)
    return QStringLiteral("1 активная сессия");
  return QStringLiteral("%1 активных сессий").arg(static_cast<int>(n));
}

bool NodeController::canSendMessage() const {
  if (active_chat_key_.isEmpty())
    return false;
  return service_.is_session_live(active_chat_key_.toStdString());
}

QString NodeController::dmInboxToken() const {
  return QString::fromStdString(service_.dm_inbox_token_hex());
}

QString NodeController::sessionStateForKey(const QString& key) const {
  const auto state = service_.session_state(key.toStdString());
  return QString::fromUtf8(nyx_app::session_state_name(state));
}

bool NodeController::applyRendezvousList(const QString& v) {
  const QString trimmed = v.trimmed();
  if (trimmed.isEmpty())
    return false;
  if (!service_.set_rendezvous_list(trimmed.toStdString()))
    return false;
  rendezvous_list_ = QString::fromStdString(service_.rendezvous_list_string());
  const auto primary = service_.network_config().primary_rendezvous();
  rendezvous_ =
      QString::fromStdString(primary.host) + QLatin1Char(':') + QString::number(primary.port);
  emit rendezvousChanged();
  return true;
}

void NodeController::setRendezvous(const QString& v) {
  const QString trimmed = v.trimmed();
  if (rendezvous_ == trimmed)
    return;
  if (!applyRendezvousList(trimmed)) {
    network_status_ = QStringLiteral("Неверный формат rendezvous (host:port)");
    emit networkSettingsChanged();
    return;
  }
  saveNetworkSettings();
}

void NodeController::setRendezvousList(const QString& v) {
  const QString trimmed = v.trimmed();
  if (rendezvous_list_ == trimmed)
    return;
  if (!applyRendezvousList(trimmed)) {
    network_status_ = QStringLiteral("Неверный формат (host:port,…)");
    emit networkSettingsChanged();
    return;
  }
}

void NodeController::setDiscoveryMode(int mode) {
  if (discovery_mode_ == mode)
    return;
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

void NodeController::setNickname(const QString& v) {
  const QString n = v.trimmed();
  if (n == profile_nickname_)
    return;
  service_.set_nickname(n.toStdString());
  refreshProfile();
  showToast(QStringLiteral("Личная информация обновлена"));
}

void NodeController::setConnectionPanelOpen(bool open) {
  if (connection_panel_open_ == open)
    return;
  connection_panel_open_ = open;
  emit connectionPanelOpenChanged();
}

void NodeController::setFieldInfoOpen(bool open) {
  if (field_info_open_ == open)
    return;
  field_info_open_ = open;
  emit fieldInfoOpenChanged();
}

void NodeController::setPeerInfoOpen(bool open) {
  if (peer_info_open_ == open)
    return;
  peer_info_open_ = open;
  emit peerInfoOpenChanged();
}

void NodeController::syncFieldInfoState() {
  field_info_invite_.clear();
  field_info_is_owner_ = false;
  field_info_description_.clear();
  field_info_direction_.clear();
  field_info_tags_.clear();
  field_info_public_listed_ = false;
  field_info_members_.clear();
  const QString gid = field_info_group_id_.trimmed().toLower();
  if (gid.isEmpty())
    return;

  auto apply_map = [this](const QVariantMap& m) {
    field_info_invite_ = m.value(QStringLiteral("invite")).toString().trimmed();
    field_info_is_owner_ = m.value(QStringLiteral("isOwner")).toBool();
    field_info_description_ = m.value(QStringLiteral("description")).toString();
    field_info_direction_ = m.value(QStringLiteral("direction")).toString();
    field_info_tags_ = m.value(QStringLiteral("tags")).toString();
    field_info_public_listed_ = m.value(QStringLiteral("publicListed")).toBool();
    field_info_members_ = m.value(QStringLiteral("members")).toList();
  };

  for (const auto& item : group_list_) {
    const QVariantMap m = item.toMap();
    if (m.value(QStringLiteral("groupId")).toString().trimmed().toLower() != gid)
      continue;
    apply_map(m);
    return;
  }

  nyx::GroupId id {};
  if (!nyx::GroupStore::group_id_from_hex(gid.toStdString(), id))
    return;
  nyx::Profile profile;
  const bool have_profile = nyx::active_profile(profile);
  for (const auto& g : service_.list_groups()) {
    if (g.id != id)
      continue;
    field_info_invite_ = QString::fromStdString(nyx::GroupStore::invite_hex(g.invite_token));
    field_info_description_ = QString::fromStdString(g.description);
    field_info_direction_ = QString::fromStdString(g.direction);
    field_info_tags_ = QString::fromStdString(g.tags);
    field_info_public_listed_ = g.visibility == nyx::GroupVisibility::PublicListed;
    if (have_profile) {
      field_info_is_owner_ = g.owner_id == profile.user_id();
      if (!field_info_is_owner_) {
        for (const auto& mem : g.members) {
          if (mem.role == nyx::GroupRole::Owner && mem.user_id == profile.user_id()) {
            field_info_is_owner_ = true;
            break;
          }
        }
      }
    }
    for (const auto& member : g.members) {
      QVariantMap mm;
      const QString uid =
          QString::fromStdString(nyx::to_hex(member.user_id.data(), member.user_id.size()));
      mm.insert(QStringLiteral("userId"), uid);
      mm.insert(QStringLiteral("nickname"), QString::fromStdString(member.nickname));
      mm.insert(QStringLiteral("isOwner"), member.role == nyx::GroupRole::Owner);
      mm.insert(QStringLiteral("isHost"), member.role == nyx::GroupRole::Host);
      mm.insert(QStringLiteral("role"),
                member.role == nyx::GroupRole::Owner
                    ? QStringLiteral("owner")
                    : (member.role == nyx::GroupRole::Host ? QStringLiteral("host")
                                                           : QStringLiteral("member")));
      mm.insert(QStringLiteral("idShort"),
                QString::fromStdString(nyx::short_user_id(member.user_id)));
      field_info_members_.append(mm);
    }
    return;
  }
}

void NodeController::openFieldInfo(const QString& groupIdHex) {
  QString gid = groupIdHex.trimmed().toLower();
  if (gid.isEmpty())
    gid = active_chat_ref_id_.trimmed().toLower();
  if (gid.isEmpty()) {
    showToast(QStringLiteral("Поле не выбрано"), true);
    return;
  }
  field_info_group_id_ = gid;
  refreshGroupList();
  syncFieldInfoState();
  field_info_open_ = true;
  emit fieldInfoOpenChanged();
}

void NodeController::openPeerInfo(const QString& userIdHex) {
  QString uid = userIdHex.trimmed().toLower();
  if (uid.isEmpty())
    uid = active_chat_ref_id_.trimmed().toLower();
  if (uid.isEmpty()) {
    showToast(QStringLiteral("Собеседник не выбран"), true);
    return;
  }
  peer_info_user_id_ = uid;
  refreshContactList();
  peer_info_open_ = true;
  emit peerInfoOpenChanged();
}

void NodeController::setMainViewMode(int mode) {
  const int m = mode == 1 ? 1 : 0;
  if (main_view_mode_ == m)
    return;
  main_view_mode_ = m;
  if (m == 1) {
    if (in_chat_ && active_chat_kind_ == static_cast<int>(nyx::ConversationKind::Group) &&
        !active_chat_ref_id_.isEmpty()) {
      setFileScopeGroupId(active_chat_ref_id_);
    }
    if (!active_chat_key_.isEmpty() && active_chat_key_.startsWith(QStringLiteral("group:"))) {
      service_.set_active_session(active_chat_key_.toStdString());
    }
    refreshGroupList();
    refreshFileLists();
    refreshFileAccessLists();
    if (fileExchangeReady())
      refreshRemoteFileList();
  }
  emit mainViewModeChanged();
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
  const QString relative = mediaRelativeDir(active_chat_key_, peer_title_, kind);
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
  std::thread([this, p, scope, rescan]() {
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

void NodeController::wireCallbacks() {
  wireStatusCallbacks();
  wireChatCallbacks();
  wireSessionCallbacks();
  wireDiscoveryCallbacks();
  wireGroupCallbacks();
  wireCallCallbacks();
  wireFileCallbacks();
}

void NodeController::wireStatusCallbacks() {
  service_.set_on_status([this](const std::string& text) {
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
          const QString q = QString::fromStdString(text);
          setStatus(q);
          const QString lower = q.toLower();
          if (lower.contains(QStringLiteral("вошёл")) || lower.contains(QStringLiteral("вошел"))) {
            refreshGroupList();
            if (main_view_mode_ == 1)
              refreshFileAccessLists();
          }

          const bool progress_noise = lower.contains(QStringLiteral("lookup")) ||
                                      lower.contains(QStringLiteral("rendezvous")) ||
                                      lower.contains(QStringLiteral("register")) ||
                                      lower.contains(QStringLiteral("handshake")) ||
                                      lower.contains(QStringLiteral("bind ")) ||
                                      lower.startsWith(QStringLiteral("hub «")) ||
                                      lower.startsWith(QStringLiteral("эфир «")) ||
                                      lower.contains(QStringLiteral("invite:")) ||
                                      lower.contains(QStringLiteral("подключение к hub")) ||
                                      lower.contains(QStringLiteral("подключение к эфиру")) ||
                                      lower.contains(QStringLiteral("подключение к ")) ||
                                      lower.contains(QStringLiteral("поиск на rendezvous")) ||
                                      lower.contains(QStringLiteral("повтор lookup")) ||
                                      lower.contains(QStringLiteral("пробить nat")) ||
                                      lower.contains(QStringLiteral("установить канал")) ||
                                      lower.contains(QStringLiteral("собеседник не найден")) ||
                                      lower.contains(QStringLiteral("эфир не найден")) ||
                                      lower.contains(QStringLiteral("поле недоступно")) ||
                                      lower.contains(QStringLiteral("владелец офлайн"));
          if (!progress_noise && (lower.contains(QStringLiteral("failed")) ||
                                  lower.contains(QStringLiteral("не удалось")) ||
                                  lower.contains(QStringLiteral("неверн")) ||
                                  lower.contains(QStringLiteral("отказ")) ||
                                  lower.contains(QStringLiteral("файл сохранён")) ||
                                  lower.contains(QStringLiteral("приём")) ||
                                  lower.contains(QStringLiteral("запрос файла")) ||
                                  lower.contains(QStringLiteral("timeout")) ||
                                  lower.contains(QStringLiteral("не найден")) ||
                                  lower.contains(QStringLiteral("не отвечает")))) {
            showToast(q,
                      lower.contains(QStringLiteral("отказ")) ||
                          lower.contains(QStringLiteral("failed")) ||
                          lower.contains(QStringLiteral("не удалось")) ||
                          lower.contains(QStringLiteral("не найден")) ||
                          lower.contains(QStringLiteral("не отвечает")));
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
}

void NodeController::wireChatCallbacks() {
  service_.set_on_message([this](const nyx_app::UiMessage& msg) {
    if (!msg.outgoing) {
      const auto blocks = nyx::parse_markdown_blocks(msg.text);
      const QString chat_key = QString::fromStdString(msg.chat_key);
      const QString title = QString::fromStdString(msg.author);
      nyx::GroupId scope_id {};
      QString scope_hex;
      if (chat_key.startsWith(QLatin1String("group:"))) {
        scope_hex = chat_key.section(QLatin1Char(':'), 1).toLower();
        nyx::GroupStore::group_id_from_hex(scope_hex.toStdString(), scope_id);
      }
      const QString library_root =
          QString::fromStdString(nyx::FileIndex::library_root_path(scope_id));
      for (const auto& block : blocks) {
        if (block.type != nyx::MdBlockType::File || block.hash.empty())
          continue;
        const QString mime = QString::fromStdString(block.mime).toLower();
        const QString name = safeMediaPathPart(QString::fromStdString(block.caption));
        QString kind;
        if (mime.startsWith(QLatin1String("audio/")) &&
            (name.startsWith(QLatin1String("voice-message"), Qt::CaseInsensitive) ||
             name.compare(QLatin1String("voice.m4a"), Qt::CaseInsensitive) == 0)) {
          kind = QStringLiteral("voice");
        } else if (mime.startsWith(QLatin1String("video/")) &&
                   name.startsWith(QLatin1String("circle-message"), Qt::CaseInsensitive)) {
          kind = QStringLiteral("circle");
        } else {
          continue;
        }
        const QString relative_dir = mediaRelativeDir(chat_key, title, kind);
        const QString dest_dir = QDir(library_root).filePath(relative_dir);
        QDir().mkpath(dest_dir);
        const QString destination = QDir(dest_dir).filePath(
            QString::fromStdString(block.hash).left(12) + QLatin1Char('-') + name);

        if (const auto local = service_.find_file_object(block.hash)) {
          service_.import_file_object(local->absolute_path(),
                                      name.toStdString(),
                                      mime.toStdString(),
                                      scope_hex.toStdString(),
                                      msg.author_user_id,
                                      relative_dir.toStdString());
        } else {
          service_.download_file(block.hash, destination.toStdString(), msg.session_id);
        }
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, msg]() {
          const QString chat_key = QString::fromStdString(msg.chat_key);
          const bool for_active = chat_key.isEmpty() || chat_key == active_chat_key_ ||
                                  (msg.session_id == service_.active_session_id());
          if (for_active) {
            if (msg.message_id != 0 && messages_.hasMessageId(msg.message_id)) {
              if (!msg.delivery.empty()) {
                messages_.setDelivery(msg.message_id, QString::fromStdString(msg.delivery));
              }
            } else {
              messages_.appendMessage(QString::fromStdString(msg.author),
                                      QString::fromStdString(msg.text),
                                      msg.outgoing,
                                      msg.timestamp_ms,
                                      msg.message_id,
                                      QString::fromStdString(msg.delivery),
                                      QString::fromStdString(msg.author_user_id));
            }
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
                  QSystemTrayIcon::Information,
                  4000);
            }
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_delivery([this](const std::string&, uint64_t message_id, bool delivered) {
    QMetaObject::invokeMethod(
        this,
        [this, message_id, delivered]() {
          messages_.setDelivery(message_id,
                                delivered ? QStringLiteral("delivered") : QStringLiteral("failed"));
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireSessionCallbacks() {
  service_.set_on_chat_ready([this](const std::string& session_id,
                                    const std::string& peer_title,
                                    const std::string& conn_label,
                                    nyx::ConversationKind kind,
                                    const std::string& ref_id) {
    QMetaObject::invokeMethod(
        this,
        [this, session_id, peer_title, conn_label, kind, ref_id]() {
          const QString sid = QString::fromStdString(session_id);
          const QString ref = QString::fromStdString(ref_id);
          const QString list_key = kind == nyx::ConversationKind::Group
                                       ? (QStringLiteral("group:") + ref)
                                       : (ref.isEmpty() ? sid : QStringLiteral("dm:") + ref);

          chat_list_.setSessionState(list_key, QStringLiteral("live"));
          if (sid != list_key)
            chat_list_.setSessionState(sid, QStringLiteral("live"));

          const bool user_waiting = pending_field_join_notify_;
          const bool was_selected = !active_chat_key_.isEmpty() && active_chat_key_ == list_key;
          pending_field_join_notify_ = false;

          if (user_waiting || was_selected) {
            service_.set_active_session(session_id);
            active_chat_kind_ = static_cast<int>(kind);
            active_chat_ref_id_ = ref;
            enterChat(QString::fromStdString(peer_title),
                      QString::fromStdString(conn_label),
                      static_cast<int>(kind),
                      ref);
            if (kind == nyx::ConversationKind::Group) {
              loadStoredHistory(static_cast<int>(kind), ref, list_key);
              showToast(QStringLiteral("В поле «") + QString::fromStdString(peer_title) +
                        QStringLiteral("»"));
            }
          } else if (kind == nyx::ConversationKind::Group) {
            showToast(QStringLiteral("Эфир «") + QString::fromStdString(peer_title) +
                      QStringLiteral("» снова открыт"));
          } else {
            showToast(QStringLiteral("Собеседник снова на связи"));
          }

          refreshChatList();
          refreshGroupList();
          refreshContactList();
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
          const bool active_gone = !active_chat_key_.isEmpty() &&
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
              showToast(QStringLiteral("Эфир закрыт — владелец не в сети"), false);
            }
          }

          if (sid.startsWith(QStringLiteral("group:")) &&
              !sid.startsWith(QStringLiteral("group:join:"))) {
            const auto st = service_.session_state(session_id);
            if (st == nyx_app::SessionState::Offline &&
                service_.is_session_intent_enabled(sid.toStdString())) {
              QTimer::singleShot(3000, this, [this, sid]() {
                if (!auto_start_owned_hub_)
                  return;
                if (!service_.is_session_intent_enabled(sid.toStdString()))
                  return;
                if (service_.is_session_up(sid.toStdString()))
                  return;
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
}

void NodeController::wireDiscoveryCallbacks() {
  service_.set_on_sessions_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshChatSessionStates();
          emit sessionsChanged();
          emit busyChanged();
          emit listeningChanged();
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

  service_.set_on_mode([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          emit listeningChanged();
          emit busyChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireGroupCallbacks() {
  service_.set_on_group_created([this](const std::string& gid, const std::string& invite) {
    QMetaObject::invokeMethod(
        this,
        [this, gid, invite]() {
          last_group_invite_ = QString::fromStdString(invite);
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

  service_.set_on_group_meta_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshGroupList();
          if (field_info_open_) {
            syncFieldInfoState();
            emit fieldInfoOpenChanged();
          }
        },
        Qt::QueuedConnection);
  });

  service_.set_on_avatars_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          refreshContactList();
          emit profilePhotosChanged();
          emit chatChanged();
        },
        Qt::QueuedConnection);
  });
}

void NodeController::wireCallCallbacks() {
  service_.set_on_call_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          emit callChanged();

          syncCallNotifications();
          syncCallAudio();
        },
        Qt::QueuedConnection);
  });
  service_.set_on_call_media(
      [this](nyx::CallMediaType type, const nyx::ByteBuffer& payload, const nyx::UserId& from) {
        QByteArray packet(reinterpret_cast<const char*>(payload.data()),
                          static_cast<int>(payload.size()));
        QString peer;
        const bool from_zero =
            std::all_of(from.begin(), from.end(), [](uint8_t b) { return b == 0; });
        if (!from_zero) {
          peer = QString::fromStdString(nyx::to_hex(from.data(), from.size()));
        }
        QMetaObject::invokeMethod(
            this,
            [this, type, packet, peer]() {
              if (type == nyx::CallMediaType::Opus) {
                call_audio_.onRemoteOpus(peer, packet);
              } else if (type == nyx::CallMediaType::Video) {
                call_video_.onRemoteVideo(peer, packet);
              }
            },
            Qt::QueuedConnection);
      });
}

void NodeController::wireFileCallbacks() {
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
              if (finished)
                return;
              file_index_progress_visible_ = true;

              const int paced =
                  5 + static_cast<int>(
                          (95.0 * (1.0 - std::exp(-static_cast<double>(files_scanned) / 80.0))));
              file_index_progress_percent_ = qBound(5, paced, 95);
              const QString name = QString::fromStdString(path);
              file_index_progress_label_ =
                  name.isEmpty() ? QStringLiteral("Сканирование… %1 файлов").arg(files_scanned)
                                 : QStringLiteral("%1 · %2").arg(name).arg(files_scanned);
              emit fileIndexProgressChanged();
            },
            Qt::QueuedConnection);
      });
  service_.set_on_transfer_queue_changed([this]() {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          transfer_queue_.clear();
          for (const auto& task : service_.transfer_queue()) {
            QVariantMap item;
            item.insert(QStringLiteral("hash"), QString::fromStdString(task.hash_hex));
            const QString dest = QString::fromStdString(task.dest_path);
            item.insert(QStringLiteral("name"),
                        QFileInfo(dest).fileName().isEmpty() ? dest : QFileInfo(dest).fileName());
            item.insert(QStringLiteral("path"), dest);
            item.insert(QStringLiteral("state"), QString::fromStdString(task.state));
            item.insert(QStringLiteral("progress"), task.progress);
            item.insert(QStringLiteral("paused"), task.paused);
            item.insert(QStringLiteral("error"), QString::fromStdString(task.error));
            item.insert(QStringLiteral("direction"), QString::fromStdString(task.direction));
            transfer_queue_.append(item);
          }
          emit filesChanged();
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

bool NodeController::createAccount(const QString& nickname,
                                   const QString& password,
                                   const QString& confirmPassword,
                                   bool rememberMe) {
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
  if (!nyx::create_account(
          nickname.trimmed().toStdString(), password.toStdString(), &phrase, nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  if (rememberMe)
    nyx::enable_remember_me(nullptr);
  pending_recovery_phrase_ = QString::fromStdString(phrase);
  finishAccountUnlock(false);
  emit accountGateChanged();
  return true;
}

bool NodeController::unlockAccount(const QString& accountId,
                                   const QString& password,
                                   bool rememberMe) {
  std::string err;
  if (!nyx::unlock_account(
          accountId.toStdString(), password.toStdString(), rememberMe, nullptr, &err)) {
    account_gate_error_ = QString::fromStdString(err);
    emit accountGateChanged();
    return false;
  }
  pending_recovery_phrase_.clear();
  finishAccountUnlock(true);
  return true;
}

bool NodeController::tryUnlockRemembered(const QString& accountId) {
  if (accountId.trimmed().isEmpty())
    return false;
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
  if (!nyx::reset_password_with_recovery(
          accountId.toStdString(), recoveryPhrase.toStdString(), newPassword.toStdString(), &err)) {
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
  if (pending_recovery_phrase_.isEmpty())
    return;
  pending_recovery_phrase_.clear();
  finishAccountUnlock(true);
  emit accountGateChanged();
}

void NodeController::copyRecoveryPhrase() {
  if (pending_recovery_phrase_.isEmpty())
    return;
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
#if defined(Q_OS_ANDROID)
  nyx_android::stop_keepalive_service();
  nyx_android::cancel_call_notifications();
#endif
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

void NodeController::refreshProfile() {
  const auto profile = service_.profile();
  profile_nickname_ = QString::fromStdString(profile.nickname);
  profile_id_short_ = QString::fromStdString(nyx::short_user_id(profile.user_id()));
  profile_user_id_hex_ =
      QString::fromStdString(nyx::to_hex(profile.user_id().data(), profile.user_id().size()));
  loadProfileMeta();
  emit profileChanged();
}

void NodeController::refreshChatList() {
  chat_list_.refreshFromDisk(profile_id_short_);
  refreshChatSessionStates();
}

void NodeController::refreshChatSessionStates() {
  auto rank = [](const QString& state) -> int {
    if (state == QLatin1String("live"))
      return 3;
    if (state == QLatin1String("connecting"))
      return 2;
    if (state == QLatin1String("offline") || state == QLatin1String("disconnected"))
      return 1;
    return 0;
  };
  auto put_state = [&](QHash<QString, QString>& map, const QString& key, const QString& state) {
    if (key.isEmpty())
      return;
    const QString norm = normalizeSessionKey(key);
    const auto it = map.constFind(norm);
    if (it == map.cend() || rank(state) >= rank(it.value()))
      map.insert(norm, state);
  };

  QHash<QString, QString> live_states;
  for (const auto& info : service_.list_sessions()) {
    if (info.kind == nyx_app::SessionKind::DmInbox)
      continue;
    const QString sid = QString::fromStdString(info.id);
    const QString state = QString::fromUtf8(nyx_app::session_state_name(info.state));
    put_state(live_states, sid, state);
    const QString ref = QString::fromStdString(info.ref_id_hex).trimmed().toLower();
    if (ref.isEmpty())
      continue;
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
    if (key.isEmpty())
      continue;
    const QString norm = normalizeSessionKey(key);
    if (live_states.contains(norm)) {
      chat_list_.setSessionState(key, live_states.value(norm));
    } else {
      chat_list_.setSessionState(key, QStringLiteral("offline"));
    }
  }
}

void NodeController::setSidebarMode(int mode) {
  if (mode < 0)
    mode = 0;
  if (mode > 2)
    mode = 2;
  if (sidebar_mode_ == mode)
    return;
  sidebar_mode_ = mode;
  if (mode == 1)
    refreshContactList();
  if (mode == 2)
    refreshGroupList();
  emit sidebarModeChanged();
}

QString NodeController::shortInviteCode(const QString& hex) const {
  QString t = normalizeInviteHex(hex);
  if (t.size() <= 14)
    return t;
  return t.left(8) + QStringLiteral("…") + t.right(4);
}

void NodeController::refreshContactList() {
  contact_list_.clear();
  nyx::Profile profile;
  if (!nyx::active_profile(profile)) {
    emit contactListChanged();
    return;
  }
  nyx::ContactBook book(nyx::default_contacts_path());
  book.load();
  const uint64_t now = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
  for (const auto& c : book.contacts()) {
    if (c.user_id == profile.user_id())
      continue;
    QVariantMap m;
    const QString uid = QString::fromStdString(nyx::to_hex(c.user_id.data(), c.user_id.size()));
    m.insert(QStringLiteral("userId"), uid);
    m.insert(QStringLiteral("nickname"),
             c.nickname.empty() ? QString::fromStdString(nyx::short_user_id(c.user_id))
                                : QString::fromStdString(c.nickname));
    m.insert(QStringLiteral("idShort"), QString::fromStdString(nyx::short_user_id(c.user_id)));
    m.insert(QStringLiteral("lastSeen"),
             QString::fromStdString(nyx::format_last_seen(c.last_seen_ms, now)));
    m.insert(QStringLiteral("hasInvite"), c.dm_inbox_token_hex.size() == 64);
    m.insert(QStringLiteral("chatKey"), QStringLiteral("dm:") + uid.toLower());
    m.insert(QStringLiteral("bio"), QString::fromStdString(c.bio));
    m.insert(QStringLiteral("interests"), QString::fromStdString(c.interests));
    m.insert(QStringLiteral("availability"),
             QString::fromStdString(nyx::availability_to_string(c.availability)));
    m.insert(QStringLiteral("availabilityLabel"),
             QString::fromStdString(nyx::availability_label_ru(c.availability)));
    QStringList photo_paths;
    nyx::AvatarStore avatars;
    avatars.load();
    for (const auto& hex : c.photo_hashes) {
      nyx::FileHash h {};
      if (!nyx::hash_from_hex(hex, h))
        continue;
      const std::string p = avatars.peer_path(c.user_id, h);
      if (!p.empty() && QFileInfo::exists(QString::fromStdString(p)))
        photo_paths.append(QString::fromStdString(p));
    }
    m.insert(QStringLiteral("avatarPath"), photo_paths.isEmpty() ? QString() : photo_paths.first());
    m.insert(QStringLiteral("photoPaths"), photo_paths);
    const QString key = QStringLiteral("dm:") + uid.toLower();
    m.insert(
        QStringLiteral("sessionState"),
        QString::fromUtf8(nyx_app::session_state_name(service_.session_state(key.toStdString()))));
    contact_list_.append(m);
  }
  emit contactListChanged();
}

void NodeController::refreshProfilePhotos() {
  profile_photo_list_.clear();
  profile_avatar_path_.clear();
  nyx::AvatarStore store;
  store.load();
  for (const auto& e : store.photos()) {
    QVariantMap m;
    const QString path = QString::fromStdString(store.path_for(e.hash));
    m.insert(QStringLiteral("hash"), QString::fromStdString(nyx::hash_hex(e.hash)));
    m.insert(QStringLiteral("path"), path);
    m.insert(QStringLiteral("mime"), QString::fromStdString(e.mime));
    m.insert(QStringLiteral("setMs"), static_cast<qulonglong>(e.set_ms));
    profile_photo_list_.append(m);
    if (profile_avatar_path_.isEmpty() && !path.isEmpty())
      profile_avatar_path_ = path;
  }
  emit profilePhotosChanged();
}

void NodeController::pickAndSetProfilePhoto() {
  const QString src =
      QFileDialog::getOpenFileName(nullptr,
                                   QStringLiteral("Фото профиля"),
                                   QString(),
                                   QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
  if (src.isEmpty())
    return;
  QImage img(src);
  if (img.isNull()) {
    showToast(QStringLiteral("Не удалось открыть изображение"), true);
    return;
  }
  img = img.convertToFormat(QImage::Format_RGB32);
  if (img.width() > 512 || img.height() > 512) {
    img = img.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  QTemporaryFile tmp(QDir::temp().filePath(QStringLiteral("nyx-avatar-XXXXXX.jpg")));
  tmp.setAutoRemove(false);
  if (!tmp.open()) {
    showToast(QStringLiteral("Не удалось сохранить фото"), true);
    return;
  }
  const QString tmp_path = tmp.fileName();
  tmp.close();
  if (!img.save(tmp_path, "JPG", 85)) {
    showToast(QStringLiteral("Не удалось сжать фото"), true);
    return;
  }
  nyx::AvatarStore store;
  store.load();
  if (!store.set_from_file(tmp_path.toStdString())) {
    QFile::remove(tmp_path);
    showToast(QStringLiteral("Фото слишком большое или повреждено (макс. 200 КБ)"), true);
    return;
  }
  QFile::remove(tmp_path);
  refreshProfilePhotos();
  showToast(QStringLiteral("Фото профиля обновлено"));
}

void NodeController::makeProfilePhotoCurrent(const QString& hashHex) {
  nyx::FileHash h {};
  if (!nyx::hash_from_hex(hashHex.trimmed().toStdString(), h))
    return;
  nyx::AvatarStore store;
  store.load();
  if (!store.make_current(h)) {
    showToast(QStringLiteral("Фото не найдено"), true);
    return;
  }
  refreshProfilePhotos();
}

void NodeController::removeProfilePhoto(const QString& hashHex) {
  nyx::FileHash h {};
  if (!nyx::hash_from_hex(hashHex.trimmed().toStdString(), h))
    return;
  nyx::AvatarStore store;
  store.load();
  if (!store.remove(h)) {
    showToast(QStringLiteral("Не удалось удалить фото"), true);
    return;
  }
  refreshProfilePhotos();
}

QString NodeController::peerAvatarPath(const QString& userIdHex) const {
  const QString uid = userIdHex.trimmed().toLower();

  if (!uid.isEmpty() && uid == profile_user_id_hex_.trimmed().toLower() &&
      !profile_avatar_path_.isEmpty()) {
    return profile_avatar_path_;
  }
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid)
      return m.value(QStringLiteral("avatarPath")).toString();
  }
  nyx::UserId id {};
  nyx::ByteBuffer bytes;
  if (!nyx::from_hex(uid.toStdString(), bytes) || bytes.size() != id.size())
    return {};
  std::memcpy(id.data(), bytes.data(), id.size());
  nyx::ContactBook book(nyx::default_contacts_path());
  book.load();
  nyx::AvatarStore store;
  store.load();
  for (const auto& c : book.contacts()) {
    if (c.user_id != id)
      continue;
    for (const auto& hex : c.photo_hashes) {
      nyx::FileHash h {};
      if (!nyx::hash_from_hex(hex, h))
        continue;
      const std::string p = store.peer_path(id, h);
      if (QFileInfo::exists(QString::fromStdString(p)))
        return QString::fromStdString(p);
    }
  }
  return {};
}

QVariantList NodeController::peerAvatarHistory(const QString& userIdHex) const {
  QVariantList out;
  const QString uid = userIdHex.trimmed().toLower();
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() != uid)
      continue;
    const QStringList paths = m.value(QStringLiteral("photoPaths")).toStringList();
    for (const QString& p : paths)
      out.append(p);
    return out;
  }
  return out;
}

void NodeController::loadProfileMeta() {
  nyx::ProfileMeta meta;
  nyx::load_profile_meta(meta);
  profile_bio_ = QString::fromStdString(meta.bio);
  profile_interests_ = QString::fromStdString(meta.interests);
  profile_availability_ = QString::fromStdString(nyx::availability_to_string(meta.availability));
  refreshProfilePhotos();
  emit profileMetaChanged();
}

void NodeController::persistProfileMeta() {
  nyx::ProfileMeta meta;
  meta.bio = profile_bio_.toStdString();
  meta.interests = profile_interests_.toStdString();
  meta.availability = nyx::availability_from_string(profile_availability_.toStdString());
  meta.updated_ms = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
  if (!nyx::save_profile_meta(meta)) {
    showToast(QStringLiteral("Не удалось сохранить профиль"), true);
    return;
  }
  emit profileMetaChanged();
  showToast(QStringLiteral("Личная информация обновлена"));
}

void NodeController::setProfileBio(const QString& v) {
  if (profile_bio_ == v)
    return;
  profile_bio_ = v;
  persistProfileMeta();
}

void NodeController::setProfileInterests(const QString& v) {
  if (profile_interests_ == v)
    return;
  profile_interests_ = v;
  persistProfileMeta();
}

void NodeController::setProfileAvailability(const QString& v) {
  const QString norm = v.trimmed().toLower();
  if (profile_availability_ == norm)
    return;
  profile_availability_ = norm.isEmpty() ? QStringLiteral("available") : norm;
  persistProfileMeta();
}

QString NodeController::profileAvailabilityLabel() const {
  return QString::fromStdString(nyx::availability_label_ru(
      nyx::availability_from_string(profile_availability_.toStdString())));
}

QVariantMap NodeController::contactInfo(const QString& userIdHex) const {
  const QString uid = userIdHex.trimmed().toLower();
  if (!uid.isEmpty() && uid == profile_user_id_hex_.trimmed().toLower()) {
    QVariantMap self;
    self.insert(QStringLiteral("userId"), uid);
    self.insert(QStringLiteral("nickname"), profile_nickname_);
    self.insert(QStringLiteral("idShort"), profile_id_short_);
    self.insert(QStringLiteral("bio"), profile_bio_);
    self.insert(QStringLiteral("interests"), profile_interests_);
    self.insert(QStringLiteral("availabilityLabel"), profileAvailabilityLabel());
    self.insert(QStringLiteral("avatarPath"), profile_avatar_path_);
    QStringList paths;
    for (const QVariant& v : profile_photo_list_) {
      const QVariantMap m = v.toMap();
      const QString p = m.value(QStringLiteral("path")).toString();
      if (!p.isEmpty())
        paths.append(p);
    }
    self.insert(QStringLiteral("photoPaths"), paths);
    self.insert(QStringLiteral("isSelf"), true);
    return self;
  }
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid) {
      QVariantMap out = m;
      out.insert(QStringLiteral("isSelf"), false);
      if (!out.contains(QStringLiteral("avatarPath")) ||
          out.value(QStringLiteral("avatarPath")).toString().isEmpty()) {
        out.insert(QStringLiteral("avatarPath"), peerAvatarPath(uid));
      }
      return out;
    }
  }
  QVariantMap empty;
  empty.insert(QStringLiteral("userId"), uid);
  empty.insert(QStringLiteral("nickname"), uid.left(8));
  empty.insert(QStringLiteral("bio"), QString());
  empty.insert(QStringLiteral("interests"), QString());
  empty.insert(QStringLiteral("availabilityLabel"), QStringLiteral("неизвестно"));
  empty.insert(QStringLiteral("avatarPath"), peerAvatarPath(uid));
  empty.insert(QStringLiteral("photoPaths"), peerAvatarHistory(uid));
  empty.insert(QStringLiteral("isSelf"), false);
  return empty;
}

void NodeController::openContact(const QString& userIdHex) {
  const QString uid = userIdHex.trimmed().toLower();
  if (uid.size() != 64) {
    showToast(QStringLiteral("Неверный id контакта"), true);
    return;
  }
  QString nick = uid.left(8);
  QString last_seen;
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() != uid)
      continue;
    nick = m.value(QStringLiteral("nickname")).toString();
    last_seen = m.value(QStringLiteral("lastSeen")).toString();
    break;
  }
  const QString key = QStringLiteral("dm:") + uid;
  setSidebarMode(0);
  showChatView();
  openConversation(key, 0, uid, nick, last_seen);
  if (!service_.is_session_live(key.toStdString())) {
    if (!service_.ensure_session(key.toStdString())) {
      showToast(QStringLiteral("Нет кода для связи — попросите новый invite"), true);
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
        (g.owner_id == profile.user_id()) ? profile.nickname : std::string {};
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
    m.insert(QStringLiteral("hubOnline"), service_.is_group_hub_running(gid.toStdString()));
    m.insert(QStringLiteral("description"), QString::fromStdString(g.description));
    m.insert(QStringLiteral("direction"), QString::fromStdString(g.direction));
    m.insert(QStringLiteral("tags"), QString::fromStdString(g.tags));
    m.insert(QStringLiteral("publicListed"), g.visibility == nyx::GroupVisibility::PublicListed);

    QVariantList members;
    for (const auto& member : g.members) {
      QVariantMap mm;
      const QString uid =
          QString::fromStdString(nyx::to_hex(member.user_id.data(), member.user_id.size()));
      mm.insert(QStringLiteral("userId"), uid);
      mm.insert(QStringLiteral("nickname"), QString::fromStdString(member.nickname));
      mm.insert(QStringLiteral("isOwner"), member.role == nyx::GroupRole::Owner);
      mm.insert(QStringLiteral("isHost"), member.role == nyx::GroupRole::Host);
      mm.insert(QStringLiteral("role"),
                member.role == nyx::GroupRole::Owner
                    ? QStringLiteral("owner")
                    : (member.role == nyx::GroupRole::Host ? QStringLiteral("host")
                                                           : QStringLiteral("member")));
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

void NodeController::loadStoredHistory(int kind, const QString& refId, const QString& convKey) {
  messages_.clear();
  const auto profile = service_.profile();
  std::string path;

  const std::string key = convKey.toStdString();
  if (key.rfind("chat:", 0) == 0) {
    path = nyx::data_dir() + "/chats/" + key.substr(5) + ".jsonl";
  } else if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
    nyx::GroupId gid {};
    if (!nyx::GroupStore::group_id_from_hex(refId.toStdString(), gid))
      return;
    path = nyx::MessageStore::path_for_group(gid);
  } else {
    nyx::UserId peer {};
    if (!parse_user_id_hex(refId, peer))
      return;
    path = nyx::MessageStore::path_for_chat(nyx::dm_chat_id(profile.user_id(), peer));
  }

  nyx::MessageStore store(path);
  for (const auto& stored : store.recent(100)) {
    messages_.appendMessage(QString::fromStdString(stored.author),
                            QString::fromStdString(stored.text),
                            stored.outgoing,
                            stored.timestamp_ms,
                            stored.id,
                            stored.outgoing ? QStringLiteral("delivered") : QString(),
                            QString::fromStdString(stored.author_id_hex));
  }
}

void NodeController::openConversation(const QString& key,
                                      int kind,
                                      const QString& refId,
                                      const QString& title,
                                      const QString& lastSeen) {
  const bool live = service_.is_session_live(key.toStdString());
  const bool is_group = kind == static_cast<int>(nyx::ConversationKind::Group);

  active_chat_kind_ = kind;
  active_chat_ref_id_ = refId;
  const bool owner_field = is_group && activeFieldIsOwner();

  active_chat_key_ = key;
  peer_title_ = title;
  peer_connection_label_.clear();
  service_.set_active_session(key.toStdString());
  chat_list_.setSelectedKey(key);

  in_chat_ = true;
  if (live) {
    peer_status_text_ = is_group ? QStringLiteral("эфир открыт") : QStringLiteral("на связи");
  } else if (owner_field) {
    peer_status_text_ = QStringLiteral("открытие эфира…");
  } else if (is_group) {
    peer_status_text_ = QStringLiteral("подключение к эфиру…");
  } else {
    peer_status_text_ = lastSeen.isEmpty() ? QStringLiteral("не на связи") : lastSeen;
  }

  loadStoredHistory(kind, refId, key);
  chat_list_.clearUnread(key);
  emit chatChanged();

  QTimer::singleShot(0, this, [this]() { emit filesChanged(); });

  if (live)
    return;

  if (!owner_field) {
    if (is_group) {

      peer_status_text_ = QStringLiteral("подключение к эфиру…");
      chat_list_.setSessionState(key, QStringLiteral("connecting"));
      emit chatChanged();

      QTimer::singleShot(0, this, [this]() { connectActiveField(); });
      return;
    }
    chat_list_.setSessionState(key, QStringLiteral("offline"));
    return;
  }

  chat_list_.setSessionState(key, QStringLiteral("connecting"));
  showToast(QStringLiteral("Открываем эфир…"));
  QTimer::singleShot(0, this, [this, key]() {
    if (!service_.ensure_session(key.toStdString())) {
      peer_status_text_ = QStringLiteral("эфир закрыт");
      chat_list_.setSessionState(key, QStringLiteral("offline"));
      showToast(QStringLiteral("Не удалось открыть эфир"), true);
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

void NodeController::showWindow() {
  emit showMainWindow();
}

void NodeController::setStatus(const QString& text) {
  status_text_ = text;
  emit statusTextChanged();
}

void NodeController::showToast(const QString& text, bool isError) {
  if (text.isEmpty())
    return;
  toast_is_error_ = isError;

  toast_ = text;
  emit toastChanged();
}

QString NodeController::normalizeInviteHex(const QString& hex) const {
  QString t = hex.trimmed();
  if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
    t = t.mid(2);
  t.remove(QChar(' '));
  t.remove(QChar('\n'));
  t.remove(QChar('\r'));
  t.remove(QChar('\t'));
  return t.toLower();
}

void NodeController::enterChat(const QString& peerName,
                               const QString& connectionLabel,
                               int kind,
                               const QString& refId) {
  in_chat_ = true;
  peer_title_ = peerName;
  peer_connection_label_ = connectionLabel;
  active_chat_kind_ = kind;
  active_chat_ref_id_ = refId;
  if (kind == static_cast<int>(nyx::ConversationKind::Group) && !refId.isEmpty()) {
    peer_status_text_ = QStringLiteral("эфир открыт");
    active_chat_key_ = QStringLiteral("group:") + refId;
    const QString gid = refId.trimmed().toLower();
    if (file_scope_group_id_ != gid) {
      file_scope_group_id_ = gid;
      syncFileScopeLabel();
    }
  } else {
    peer_status_text_ = QStringLiteral("на связи");
    if (!refId.isEmpty())
      active_chat_key_ = QStringLiteral("dm:") + refId;
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
    peer_status_text_ = activeFieldIsOwner() ? QStringLiteral("эфир закрыт")
                                             : QStringLiteral("эфир закрыт — ждём владельца");
  } else if (was_in || peer_status_text_ == QStringLiteral("на связи") ||
             peer_status_text_ == QStringLiteral("в сети")) {
    peer_status_text_ = QStringLiteral("не на связи");
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
  peer_status_text_ = QStringLiteral("эфир");
  loadStoredHistory(active_chat_kind_, groupIdHex, active_chat_key_);
  emit chatChanged();
}

void NodeController::refreshLanPeers() {

  service_.scan_lan_peers(1200);
}

void NodeController::tickLanDiscovery() {
#if defined(Q_OS_ANDROID)

  nyx_android::acquire_multicast_lock();
  const std::string wifi = nyx_android::wifi_ipv4();
  if (!wifi.empty())
    nyx::set_lan_ipv4_override(wifi);
#endif
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

QString NodeController::chatMediaDir() {
  return QString::fromStdString(nyx::data_dir() + "/chat_media");
}

QString NodeController::chatCaptureStagingPath(const QString& extension) const {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                      QStringLiteral("/chat-capture");
  QDir().mkpath(dir);
  QString ext = extension.trimmed().toLower();
  if (ext.startsWith(QLatin1Char('.')))
    ext = ext.mid(1);
  if (ext.isEmpty())
    ext = QStringLiteral("bin");
  return QDir(dir).filePath(
      QStringLiteral("cap-%1.%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(ext));
}

void NodeController::removeStagingMedia(const QString& path) const {
  const QString staging = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                               QStringLiteral("/chat-capture"))
                              .canonicalPath();
  const QFileInfo info(path);
  const QString parent = info.dir().canonicalPath();
  if (!staging.isEmpty() && parent == staging)
    QFile::remove(info.absoluteFilePath());
}

void NodeController::requestChatCapturePermissions(bool needCamera) {
  nyx_android::request_call_permissions(
      needCamera, &NodeController::chatCapturePermissionCallback, this);
}

void NodeController::chatCapturePermissionCallback(bool micOk, bool cameraOk, void* ctx) {
  auto* self = static_cast<NodeController*>(ctx);
  if (!self)
    return;
  QMetaObject::invokeMethod(
      self,
      [self, micOk, cameraOk]() { emit self->chatCapturePermissionResult(micOk && cameraOk); },
      Qt::QueuedConnection);
}

QString NodeController::userDisplayName(const QString& userIdHex) const {
  const QString uid = userIdHex.trimmed().toLower();
  if (uid.isEmpty())
    return {};
  if (uid == profile_user_id_hex_.trimmed().toLower()) {
    const QString nick = profile_nickname_.trimmed();
    return nick.isEmpty() ? QStringLiteral("Вы") : nick;
  }
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().trimmed().toLower() == uid) {
      const QString nick = m.value(QStringLiteral("nickname")).toString().trimmed();
      if (!nick.isEmpty())
        return nick;
      break;
    }
  }
  for (const QVariant& v : group_list_) {
    const QVariantMap g = v.toMap();
    const QVariantList members = g.value(QStringLiteral("members")).toList();
    for (const QVariant& mv : members) {
      const QVariantMap m = mv.toMap();
      if (m.value(QStringLiteral("userId")).toString().trimmed().toLower() != uid)
        continue;
      const QString nick = m.value(QStringLiteral("nickname")).toString().trimmed();
      if (!nick.isEmpty())
        return nick;
    }
  }
  return uid.left(8) + QStringLiteral("…");
}

void NodeController::openInAppMedia(const QString& path,
                                    const QString& mime,
                                    const QString& title) {
  if (path.trimmed().isEmpty() || !QFileInfo::exists(path)) {
    showToast(QStringLiteral("Файл ещё не загружен"), true);
    return;
  }
  document_viewer_.close();
  in_app_media_path_ = path;
  in_app_media_mime_ = mime;
  in_app_media_title_ = title;
  in_app_media_open_ = true;
  emit inAppMediaChanged();
}

void NodeController::closeInAppMedia() {
  if (!in_app_media_open_ && in_app_media_path_.isEmpty())
    return;
  in_app_media_open_ = false;
  in_app_media_path_.clear();
  in_app_media_mime_.clear();
  in_app_media_title_.clear();
  emit inAppMediaChanged();
}

void NodeController::ensureChatMediaRootIndexed() {
  const QString dir = chatMediaDir();
  QDir().mkpath(dir);
  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  service_.index_folder(dir.toStdString(), scope.toStdString());
}

QString NodeController::importChatMediaMarkdown(const QString& localPath,
                                                const QString& mimeHint,
                                                const QString& displayName) {
  const QString src = localPath.trimmed();
  if (src.isEmpty() || !QFileInfo::exists(src)) {
    showToast(QStringLiteral("Файл захвата не найден"), true);
    return {};
  }

  QString mime = mimeHint.trimmed();
  if (mime.isEmpty()) {
    mime = QMimeDatabase().mimeTypeForFile(src).name();
  }
  QString name = displayName.trimmed();
  if (name.isEmpty())
    name = QFileInfo(src).fileName();
  if (name.isEmpty())
    name = QStringLiteral("media");

  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  else if (!file_scope_group_id_.isEmpty())
    scope = file_scope_group_id_;

  const auto object = service_.import_file_object(src.toStdString(),
                                                  name.toStdString(),
                                                  mime.toStdString(),
                                                  scope.toStdString(),
                                                  profile_user_id_hex_.toStdString());
  if (!object) {
    showToast(QStringLiteral("Не удалось сохранить медиа в библиотеку"), true);
    return {};
  }

  const QString hex = QString::fromStdString(nyx::hash_hex(object->hash));
  QDir().mkpath(chatMediaDir());
  const QString ext = QFileInfo(name).suffix().toLower();
  const QString dest = chatMediaDir() + QLatin1Char('/') + hex +
                       (ext.isEmpty() ? QString {} : (QLatin1Char('.') + ext));
  if (QFile::exists(dest))
    QFile::remove(dest);
  QFile::copy(src, dest);
  ensureChatMediaRootIndexed();
  refreshFileLists();

  QString safe_name = name;
  safe_name.replace(QLatin1Char(']'), QLatin1Char('_'));
  QString safe_mime = mime;
  safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
  if (safe_mime.isEmpty())
    safe_mime = QStringLiteral("application/octet-stream");
  return QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
      .arg(safe_name, hex, QString::number(static_cast<qulonglong>(object->size)), safe_mime);
}

bool NodeController::sendCapturedMedia(const QString& localPath,
                                       const QString& mimeHint,
                                       const QString& displayName,
                                       const QString& mediaKind) {
  if (!canSendMessage()) {
    showToast(QStringLiteral("Нет связи — медиа не отправлено"), true);
    return false;
  }
  const QString src = localPath.trimmed();
  const QFileInfo source_info(src);
  if (!source_info.isFile() || source_info.size() <= 0) {
    showToast(QStringLiteral("Запись не создана или пуста"), true);
    return false;
  }

  QString mime = mimeHint.trimmed();
  if (mime.isEmpty())
    mime = QMimeDatabase().mimeTypeForFile(src).name();
  QString name = displayName.trimmed();
  if (name.isEmpty())
    name = source_info.fileName();
  if (name.isEmpty())
    name = QStringLiteral("media");

  QString scope;
  if (active_chat_kind_ == 1)
    scope = active_chat_ref_id_;
  const QString chat_key = active_chat_key_;
  const QString owner = profile_user_id_hex_;
  const QString relative_dir =
      (mediaKind == QLatin1String("voice") || mediaKind == QLatin1String("circle"))
          ? mediaRelativeDir(chat_key, peer_title_, mediaKind)
          : QString {};

  QPointer<NodeController> guard(this);
  QThreadPool::globalInstance()->start(
      [guard, src, mime, name, scope, owner, relative_dir, chat_key]() {
        if (!guard)
          return;
        const auto object = guard->service_.import_file_object(src.toStdString(),
                                                               name.toStdString(),
                                                               mime.toStdString(),
                                                               scope.toStdString(),
                                                               owner.toStdString(),
                                                               relative_dir.toStdString());
        QFile::remove(src);

        QString markdown;
        if (object) {
          QString safe_name = name;
          safe_name.replace(QLatin1Char(']'), QLatin1Char('_'));
          QString safe_mime = mime;
          safe_mime.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9.+/_-]")));
          if (safe_mime.isEmpty())
            safe_mime = QStringLiteral("application/octet-stream");
          markdown = QStringLiteral("[%1](nyx-file:%2;size=%3;mime=%4)")
                         .arg(safe_name,
                              QString::fromStdString(nyx::hash_hex(object->hash)),
                              QString::number(static_cast<qulonglong>(object->size)),
                              safe_mime);
        }

        QMetaObject::invokeMethod(
            guard,
            [guard, markdown, chat_key]() {
              if (!guard)
                return;
              guard->refreshFileLists();
              if (markdown.isEmpty()) {
                guard->showToast(QStringLiteral("Не удалось сохранить запись"), true);
                return;
              }
              if (guard->active_chat_key_ != chat_key || !guard->canSendMessage()) {
                guard->showToast(QStringLiteral("Запись сохранена в Файлах, но чат уже закрыт"),
                                 true);
                return;
              }
              guard->sendMessage(markdown);
            },
            Qt::QueuedConnection);
      });
  return true;
}

QString NodeController::mediaLocalPath(const QString& hashHex) const {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return {};

  auto is_verified_path = [&](const QString& path) -> bool {
    if (path.isEmpty() || path.endsWith(QLatin1String(".part")))
      return false;
    if (!QFileInfo::exists(path))
      return false;
    return true;
  };

  if (const auto object = service_.find_file_object(hex.toStdString())) {
    const QString path = QString::fromStdString(object->absolute_path());
    if (is_verified_path(path))
      return path;
  }

  const QDir dir(chatMediaDir());
  const QStringList matches = dir.entryList(
      QStringList {hex + QStringLiteral(".*"), hex.left(12) + QStringLiteral("-*")}, QDir::Files);
  for (const QString& name : matches) {
    const QString path = dir.filePath(name);
    if (is_verified_path(path))
      return path;
  }

  const QString dl = QString::fromStdString(nyx::default_downloads_dir());
  const QDir dld(dl);
  const QStringList dlm = dld.entryList(
      QStringList {hex + QStringLiteral(".*"), hex.left(12) + QStringLiteral("-*")}, QDir::Files);
  for (const QString& name : dlm) {
    const QString path = dld.filePath(name);
    if (is_verified_path(path))
      return path;
  }
  return {};
}

bool NodeController::isImageMedia(const QString& hashHex) const {
  const QString path = mediaLocalPath(hashHex);
  if (path.isEmpty())
    return true;
  const QString suf = QFileInfo(path).suffix().toLower();
  return suf == QLatin1String("jpg") || suf == QLatin1String("jpeg") ||
         suf == QLatin1String("png") || suf == QLatin1String("webp") ||
         suf == QLatin1String("gif") || suf == QLatin1String("bmp");
}

void NodeController::ensureMediaAvailable(const QString& hashHex) {
  const QString hex = hashHex.trimmed().toLower();
  if (hex.size() != 64)
    return;
  if (!mediaLocalPath(hex).isEmpty())
    return;
  const QString dest = chatMediaDir() + QLatin1Char('/') + hex;
  QDir().mkpath(chatMediaDir());
  if (!service_.download_file(hex.toStdString(), dest.toStdString())) {
    showToast(QStringLiteral("Не удалось запросить медиа"), true);
  }
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

QString NodeController::pickChatMediaMarkdown() {
  const QString src = QFileDialog::getOpenFileName(
      nullptr,
      QStringLiteral("Фото или видео в сообщение"),
      QString(),
      QStringLiteral("Media (*.png *.jpg *.jpeg *.webp *.gif *.bmp *.mp4 *.webm *.mov *.mkv *.m4a "
                     "*.ogg *.mp3 *.wav)"));
  if (src.isEmpty())
    return {};

  QString work = src;
  QString ext = QFileInfo(src).suffix().toLower();
  const bool is_image = ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
                        ext == QLatin1String("png") || ext == QLatin1String("webp") ||
                        ext == QLatin1String("gif") || ext == QLatin1String("bmp");
  QTemporaryFile tmp;
  if (is_image) {
    QImage img(src);
    if (img.isNull()) {
      showToast(QStringLiteral("Не удалось открыть изображение"), true);
      return {};
    }
    if (img.width() > 1600 || img.height() > 1600)
      img = img.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    tmp.setFileTemplate(QDir::temp().filePath(QStringLiteral("nyx-media-XXXXXX.jpg")));
    tmp.setAutoRemove(false);
    if (!tmp.open()) {
      showToast(QStringLiteral("Не удалось сохранить медиа"), true);
      return {};
    }
    const QString tmp_path = tmp.fileName();
    tmp.close();
    if (!img.save(tmp_path, "JPG", 82)) {
      QFile::remove(tmp_path);
      showToast(QStringLiteral("Не удалось сжать изображение"), true);
      return {};
    }
    const qint64 sz = QFileInfo(tmp_path).size();
    if (sz > 2 * 1024 * 1024) {
      showToast(QStringLiteral("Изображение больше 2 МБ — лучше отправить файлом"), true);
    }
    work = tmp_path;
    ext = QStringLiteral("jpg");
  } else {
    const qint64 sz = QFileInfo(src).size();
    if (sz > 50 * 1024 * 1024) {
      showToast(QStringLiteral("Видео больше 50 МБ — отправьте через Файлы"), true);
      return {};
    }
  }

  const QString mime = QMimeDatabase().mimeTypeForFile(work).name();
  QString name = QFileInfo(src).fileName();
  if (is_image && !name.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive)) {
    name = QFileInfo(src).completeBaseName() + QStringLiteral(".jpg");
  }
  const QString md = importChatMediaMarkdown(work, mime, name);
  if (work != src)
    QFile::remove(work);
  return md;
}

void NodeController::sendMessage(const QString& text) {
  if (text.trimmed().isEmpty())
    return;
  if (!canSendMessage()) {
    showToast(QStringLiteral("Нет связи с собеседником — сообщение не отправлено"), true);
    return;
  }
  const std::string normalized = nyx::normalize_me_message(text.toStdString());
  ensureChatMediaRootIndexed();
  const auto blocks = nyx::parse_markdown_blocks(normalized);
  for (const auto& b : blocks) {
    if ((b.type != nyx::MdBlockType::Media && b.type != nyx::MdBlockType::File) || b.hash.empty()) {
      continue;
    }
    const QString path = fileLocalPath(QString::fromStdString(b.hash));
    if (!path.isEmpty())
      service_.send_file(path.toStdString());
  }
  if (!service_.send_message(normalized, active_chat_key_.toStdString())) {
    showToast(QStringLiteral("Не удалось отправить сообщение"), true);
  }
}

QString NodeController::callState() const {
  switch (service_.call_state()) {
  case nyx::CallState::Outgoing:
    return QStringLiteral("outgoing");
  case nyx::CallState::Incoming:
    return QStringLiteral("incoming");
  case nyx::CallState::Ringing:
    return QStringLiteral("ringing");
  case nyx::CallState::Active:
    return QStringLiteral("active");
  case nyx::CallState::Ended:
    return QStringLiteral("ended");
  case nyx::CallState::Idle:
  default:
    return QStringLiteral("idle");
  }
}

QString NodeController::callTitle() const {
  const QString t = QString::fromStdString(service_.call_title());
  return t.isEmpty() ? peer_title_ : t;
}

bool NodeController::callVideo() const {
  return service_.call_mode() == nyx::CallMode::AudioVideo;
}

bool NodeController::canStartCall() const {
  return service_.can_start_call(active_chat_key_.toStdString());
}

bool NodeController::callIsFieldRoom() const {
  return service_.call_is_field_room();
}

void NodeController::startCall(bool video) {
  const std::string key = active_chat_key_.toStdString();
  if (!service_.is_session_up(key)) {

    if (service_.ensure_session(key)) {
      showToast(QStringLiteral("Нет связи — переподключаюсь. Позвоните ещё раз через пару секунд."),
                false);
    } else {
      showToast(QStringLiteral("Нет связи с собеседником — дождитесь «на связи»"), true);
    }
    return;
  }
  if (!service_.can_start_call(key)) {
    showToast(QStringLiteral("Нет права открывать комнату (нужна роль ведущего)"), true);
    return;
  }
  if (video && CallVideoIo::listCameraDevices().isEmpty()) {
    showToast(QStringLiteral("Камера не найдена — аудиозвонок"), false);
    video = false;
  }
  struct Ctx {
    NodeController* self;
    bool video;
  };
  auto* ctx = new Ctx {this, video};
  const bool need_camera = video && !CallVideoIo::listCameraDevices().isEmpty();
  nyx_android::request_call_permissions(
      need_camera,
      [](bool mic_ok, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        bool want_video = c->video;
        delete c;
        if (!mic_ok) {
          self->showToast(QStringLiteral("Нужен доступ к микрофону"), true);
          return;
        }
        if (want_video && !cam_ok) {
          self->showToast(QStringLiteral("Нет доступа к камере — аудиозвонок"), false);
          want_video = false;
        }
        if (!self->service_.start_call(want_video, self->active_chat_key_.toStdString())) {
          self->showToast(QStringLiteral("Не удалось начать звонок"), true);
        }
      },
      ctx);
}

void NodeController::acceptCall() {
  answering_call_ = true;
#if defined(Q_OS_ANDROID)

  nyx_android::stop_ringtone();
#endif
  struct Ctx {
    NodeController* self;
  };
  auto* ctx = new Ctx {this};
  const bool video = callVideo();
  const bool need_camera = video && !CallVideoIo::listCameraDevices().isEmpty();
  nyx_android::request_call_permissions(
      need_camera,
      [](bool mic_ok, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        delete c;
        if (!mic_ok) {
          self->answering_call_ = false;
          self->last_call_notify_key_.clear();
          self->syncCallNotifications();
          self->showToast(QStringLiteral("Нужен доступ к микрофону"), true);
          return;
        }
        if (self->callVideo() && !cam_ok) {
          self->showToast(QStringLiteral("Нет доступа к камере — только приём видео"), false);
        }
        if (!self->service_.accept_call()) {
          self->answering_call_ = false;
          self->last_call_notify_key_.clear();
          self->syncCallNotifications();
          self->showToast(QStringLiteral("Не удалось войти в комнату"), true);
        }
      },
      ctx);
}

void NodeController::rejectCall() {
  service_.reject_call();
}

void NodeController::hangupCall() {
  service_.hangup_call();
}

void NodeController::syncCallAudio() {
  const auto st = service_.call_state();
  const bool video = service_.call_mode() == nyx::CallMode::AudioVideo;
  if (st == nyx::CallState::Active) {
    if (call_audio_.micTestActive())
      call_audio_.stopMicLevelTest();
#if defined(Q_OS_ANDROID)
    nyx_android::set_voip_audio_mode(true);
    nyx_android::set_speakerphone(call_speakerphone_);
#endif
    const bool mic_muted = service_.call_mic_muted();

    call_audio_.setMuted(mic_muted);
    call_audio_.setSendFn([this](const std::vector<uint8_t>& packet) {
      const bool ok =
          service_.send_call_media(nyx::CallMediaType::Opus, packet, call_audio_.localVoiceLevel());
      if (!ok) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        if (now - last_send_fail_toast_ms_ > 4000 && now - call_media_started_ms_ > 1500) {
          last_send_fail_toast_ms_ = now;
          QMetaObject::invokeMethod(
              this,
              [this]() { showToast(QStringLiteral("Аудио не уходит (нет канала)"), true); },
              Qt::QueuedConnection);
        }
      }
      return ok;
    });
    call_media_started_ms_ = QDateTime::currentMSecsSinceEpoch();
    call_audio_.start();
    if (video) {
      const bool small_field =
          service_.call_is_field_room() && service_.call_participants().size() <= 2;
      call_video_.setTransmitEnabled(!service_.call_is_field_room() || small_field ||
                                     call_audio_.localVoiceActive());
      call_video_.setSendFn([this](const QByteArray& frag) {
        const nyx::ByteBuffer buf(frag.begin(), frag.end());
        return service_.send_call_media(nyx::CallMediaType::Video, buf);
      });
      if (!call_video_slots_wired_) {
        call_video_slots_wired_ = true;
        connect(
            &call_video_, &CallVideoIo::remoteFrameChanged, this, [this](const QString& peerId) {
              const QImage img = call_video_.peerFrame(peerId);
              if (img.isNull())
                return;
              if (call_frames_) {
                call_frames_->setRemote(peerId, img);
                call_frames_->setPrimaryRemoteKey(call_video_.focusedPeerId());
              }
              if (peerId == call_video_.focusedPeerId() || call_video_.focusedPeerId().isEmpty()) {
                ++call_frame_epoch_;
                call_remote_frame_url_ =
                    QUrl(QStringLiteral("image://nyxcall/remote/%1").arg(call_frame_epoch_));
                emit callRemoteFrameChanged();
              }
              emit callVideoPeersChanged();
            });
        connect(&call_video_, &CallVideoIo::localFrameChanged, this, [this]() {
          const QImage img = call_video_.lastLocalFrame();
          if (call_frames_)
            call_frames_->setLocal(img);
          ++call_frame_epoch_;
          if (img.isNull()) {
            call_local_frame_url_.clear();
          } else {
            call_local_frame_url_ =
                QUrl(QStringLiteral("image://nyxcall/local/%1").arg(call_frame_epoch_));
          }
          emit callLocalFrameChanged();
        });
        connect(&call_video_,
                &CallVideoIo::videoPeersChanged,
                this,
                &NodeController::callVideoPeersChanged);
        connect(&call_video_, &CallVideoIo::cameraChanged, this, [this]() {
          saveMediaDevicePrefs();
          emit mediaDevicesChanged();
          emit callChanged();
        });
      }
      if (!call_video_.running()) {
#if defined(Q_OS_ANDROID)

        service_.set_call_camera_on(true);
        call_video_.setCameraEnabled(true);
        call_video_.start();
        emit callChanged();
        emit mediaDevicesChanged();
        QTimer::singleShot(800, this, [this]() {
          if (service_.call_state() != nyx::CallState::Active)
            return;
          if (service_.call_mode() != nyx::CallMode::AudioVideo)
            return;
          if (!call_video_.running())
            return;
          if (call_video_.capturing())
            return;
          showToast(QStringLiteral("Камера недоступна — только приём видео"), false);
          service_.set_call_camera_on(false);
          emit callChanged();
          emit mediaDevicesChanged();
        });
#else
        service_.set_call_camera_on(true);
        call_video_.setCameraEnabled(true);
        call_video_.start();
        emit callChanged();
        emit mediaDevicesChanged();
        if (!call_video_.capturing()) {
          QTimer::singleShot(500, this, [this]() {
            if (service_.call_state() != nyx::CallState::Active)
              return;
            if (service_.call_mode() != nyx::CallMode::AudioVideo)
              return;
            if (!call_video_.running())
              return;
            if (call_video_.capturing())
              return;
            call_video_.setCameraEnabled(true);
            emit callChanged();
            emit mediaDevicesChanged();
            if (!call_video_.capturing()) {
              showToast(QStringLiteral("Камера недоступна — только приём видео"), false);
            }
          });
        }
#endif
      }
    } else if (call_video_slots_wired_ || call_video_.running()) {
      disconnect(&call_video_, nullptr, this, nullptr);
      call_video_slots_wired_ = false;
      call_video_.stop();
      if (call_frames_)
        call_frames_->clear();
      call_remote_frame_url_.clear();
      call_local_frame_url_.clear();
      emit callRemoteFrameChanged();
      emit callLocalFrameChanged();
    }
  } else {
#if defined(Q_OS_ANDROID)
    nyx_android::set_voip_audio_mode(false);
#endif
    disconnect(&call_video_, nullptr, this, nullptr);
    call_video_slots_wired_ = false;
    call_audio_.stop();
    call_video_.stop();
    if (call_frames_)
      call_frames_->clear();
    call_remote_frame_url_.clear();
    call_local_frame_url_.clear();
    call_frame_epoch_ = 0;
    emit callRemoteFrameChanged();
    emit callLocalFrameChanged();
    emit callVideoPeersChanged();
    emit callChanged();
  }
}

QUrl NodeController::callRemoteFrameUrl() const {
  return call_remote_frame_url_;
}

QUrl NodeController::callLocalFrameUrl() const {
  return call_local_frame_url_;
}

bool NodeController::callCanSwitchCamera() const {
  return call_video_.canSwitchCamera();
}

void NodeController::setCallFrameProvider(CallFrameProvider* provider) {
  call_frames_ = provider;
}

void NodeController::loadMediaDevicePrefs() {
  QSettings s;
  s.beginGroup(QStringLiteral("callMedia"));
  const QString cam = s.value(QStringLiteral("cameraId")).toString();
  const QString ain = s.value(QStringLiteral("audioInputId")).toString();
  const QString aout = s.value(QStringLiteral("audioOutputId")).toString();
  call_speakerphone_ = s.value(QStringLiteral("speakerphone"), true).toBool();
  s.endGroup();
  if (!cam.isEmpty())
    call_video_.setPreferredCameraId(cam);
  if (!ain.isEmpty())
    call_audio_.setPreferredInputId(ain);
  if (!aout.isEmpty())
    call_audio_.setPreferredOutputId(aout);
}

void NodeController::saveMediaDevicePrefs() const {
  QSettings s;
  s.beginGroup(QStringLiteral("callMedia"));
  s.setValue(QStringLiteral("cameraId"), call_video_.preferredCameraId());
  s.setValue(QStringLiteral("audioInputId"), call_audio_.preferredInputId());
  s.setValue(QStringLiteral("audioOutputId"), call_audio_.preferredOutputId());
  s.setValue(QStringLiteral("speakerphone"), call_speakerphone_);
  s.endGroup();
}

void NodeController::refreshMediaDevices() {
  emit mediaDevicesChanged();
}

float NodeController::audioTestLevel() const {
  return call_audio_.micLevel();
}

bool NodeController::audioTestActive() const {
  return call_audio_.micTestActive();
}

void NodeController::startMicTest() {
  if (service_.call_state() == nyx::CallState::Active ||
      service_.call_state() == nyx::CallState::Outgoing ||
      service_.call_state() == nyx::CallState::Ringing ||
      service_.call_state() == nyx::CallState::Incoming) {
    showToast(QStringLiteral("Сначала завершите звонок"), true);
    return;
  }
#if defined(Q_OS_ANDROID)
  nyx_android::request_call_permissions(
      false,
      [](bool mic_ok, bool, void* ctx) {
        auto* self = static_cast<NodeController*>(ctx);
        if (!mic_ok) {
          QMetaObject::invokeMethod(
              self,
              [self]() { self->showToast(QStringLiteral("Нет доступа к микрофону"), true); },
              Qt::QueuedConnection);
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self]() {
              if (!self->call_audio_.startMicLevelTest())
                self->showToast(QStringLiteral("Не удалось открыть микрофон"), true);
            },
            Qt::QueuedConnection);
      },
      this);
#else
  if (!call_audio_.startMicLevelTest())
    showToast(QStringLiteral("Не удалось открыть микрофон"), true);
#endif
}

void NodeController::stopAudioTest() {
  call_audio_.stopMicLevelTest();
}

void NodeController::playSpeakerTest() {
  if (service_.call_state() == nyx::CallState::Active) {
    showToast(QStringLiteral("Сначала завершите звонок"), true);
    return;
  }
  call_audio_.playSpeakerTestTone();
}

QVariantList NodeController::cameraDeviceList() const {
  return CallVideoIo::listCameraDevices();
}

QVariantList NodeController::audioInputDeviceList() const {
  return CallAudioIo::listInputDevices();
}

QVariantList NodeController::audioOutputDeviceList() const {
  return CallAudioIo::listOutputDevices();
}

QString NodeController::selectedCameraId() const {
  return call_video_.preferredCameraId();
}

QString NodeController::selectedAudioInputId() const {
  return call_audio_.preferredInputId();
}

QString NodeController::selectedAudioOutputId() const {
  return call_audio_.preferredOutputId();
}

void NodeController::setSelectedCameraId(const QString& id) {
  call_video_.setPreferredCameraId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
  emit callChanged();
}

void NodeController::setSelectedAudioInputId(const QString& id) {
  call_audio_.setPreferredInputId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
}

void NodeController::setSelectedAudioOutputId(const QString& id) {
  call_audio_.setPreferredOutputId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
}

QString NodeController::resolveCallPeerName(const QString& peerIdHex) const {
  const QString uid = peerIdHex.trimmed().toLower();
  if (uid.isEmpty() || uid == QLatin1String("direct"))
    return callTitle();
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid)
      return m.value(QStringLiteral("nickname")).toString();
  }
  for (const QVariant& v : field_info_members_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid)
      return m.value(QStringLiteral("nickname")).toString();
  }
  return uid.left(8);
}

QVariantList NodeController::callVideoPeers() const {
  QVariantList out;
  for (const QString& id : call_video_.videoPeerIds()) {
    QVariantMap row;
    row.insert(QStringLiteral("userId"), id);
    row.insert(QStringLiteral("nickname"), resolveCallPeerName(id));
    row.insert(QStringLiteral("focused"), id == call_video_.focusedPeerId());
    out.append(row);
  }
  return out;
}

QVariantList NodeController::callRosterPeers() const {
  if (!callIsFieldRoom())
    return {};
  QVariantList video = callVideoPeers();
  if (!video.isEmpty())
    return video;
  QVariantList out;
  const auto self = service_.profile().public_key;
  for (const auto& participant : service_.call_participants()) {
    if (participant == self)
      continue;
    const QString id = QString::fromStdString(nyx::to_hex(participant.data(), participant.size()));
    QVariantMap row;
    row.insert(QStringLiteral("userId"), id);
    row.insert(QStringLiteral("nickname"), resolveCallPeerName(id));
    row.insert(QStringLiteral("focused"), id == call_video_.focusedPeerId());
    out.append(row);
  }
  if (!out.isEmpty())
    return out;
  for (const QVariant& v : field_info_members_) {
    const QVariantMap m = v.toMap();
    QVariantMap row;
    row.insert(QStringLiteral("userId"), m.value(QStringLiteral("userId")));
    row.insert(QStringLiteral("nickname"), m.value(QStringLiteral("nickname")));
    row.insert(QStringLiteral("focused"), false);
    out.append(row);
  }
  return out;
}

bool NodeController::callMicMuted() const {
  return service_.call_mic_muted();
}

void NodeController::setCallMicMuted(bool muted) {
  service_.set_call_mic_muted(muted);
  call_audio_.setMuted(muted);
  emit callChanged();
}

void NodeController::toggleCallMicMuted() {
  setCallMicMuted(!callMicMuted());
}

bool NodeController::callCameraOn() const {
  return service_.call_camera_on() && call_video_.cameraEnabled();
}

void NodeController::setCallCameraOn(bool on) {
  if (!on) {
    service_.set_call_camera_on(false);
    call_video_.setCameraEnabled(false);
    if (call_frames_)
      call_frames_->setLocal(QImage());
    call_local_frame_url_.clear();
    emit callLocalFrameChanged();
    emit callChanged();
    return;
  }
#if defined(Q_OS_ANDROID)
  struct Ctx {
    NodeController* self;
  };
  auto* ctx = new Ctx {this};
  nyx_android::request_call_permissions(
      true,
      [](bool, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        delete c;
        if (!cam_ok) {
          self->showToast(QStringLiteral("Нужен доступ к камере"), true);
          self->service_.set_call_camera_on(false);
          self->call_video_.setCameraEnabled(false);
          emit self->callChanged();
          return;
        }
        self->service_.set_call_camera_on(true);
        self->call_video_.setCameraEnabled(true);
        emit self->callChanged();
      },
      ctx);
#else
  service_.set_call_camera_on(true);
  call_video_.setCameraEnabled(true);
  emit callChanged();
#endif
}

void NodeController::toggleCallCamera() {
  setCallCameraOn(!callCameraOn());
}

bool NodeController::callSpeakerphone() const {
  return call_speakerphone_;
}

void NodeController::setCallSpeakerphone(bool on) {
  call_speakerphone_ = on;
#if defined(Q_OS_ANDROID)
  nyx_android::set_speakerphone(on);
#endif
  saveMediaDevicePrefs();
  emit callChanged();
}

void NodeController::toggleCallSpeakerphone() {
  setCallSpeakerphone(!callSpeakerphone());
}

void NodeController::switchCallCamera() {
  if (!call_video_.switchCamera()) {
    showToast(QStringLiteral("Другая камера недоступна"), true);
    return;
  }
  saveMediaDevicePrefs();
  emit callChanged();
  emit mediaDevicesChanged();
}

void NodeController::setCallFocusedPeer(const QString& peerIdHex) {
  const QString id = peerIdHex.trimmed().toLower();
  if (id.isEmpty())
    return;
  manual_call_focus_ = id;
  manual_call_focus_until_ms_ = QDateTime::currentMSecsSinceEpoch() + 10000;
  call_video_.setFocusedPeerId(id);
  if (call_frames_)
    call_frames_->setPrimaryRemoteKey(id);
  const QImage img = call_video_.peerFrame(id);
  if (img.isNull()) {
    emit callVideoPeersChanged();
    return;
  }
  if (call_frames_)
    call_frames_->setRemote(id, img);
  ++call_frame_epoch_;
  call_remote_frame_url_ = QUrl(QStringLiteral("image://nyxcall/remote/%1").arg(call_frame_epoch_));
  emit callRemoteFrameChanged();
  emit callVideoPeersChanged();
}

void NodeController::createGroup(const QString& name,
                                 const QString& description,
                                 const QString& direction,
                                 const QString& tags,
                                 bool publicListed) {
  const QString n = name.trimmed();
  if (n.isEmpty()) {
    showToast(QStringLiteral("Укажите название поля"), true);
    return;
  }
  if (!service_.create_group(n.toStdString())) {
    showToast(QStringLiteral("Не удалось создать поле"), true);
    return;
  }
  refreshGroupList();
  QString gid;
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("name")).toString() == n &&
        m.value(QStringLiteral("isOwner")).toBool()) {
      gid = m.value(QStringLiteral("groupId")).toString();
      break;
    }
  }
  if (!gid.isEmpty()) {
    service_.update_group_meta(gid.toStdString(),
                               description.trimmed().toStdString(),
                               direction.trimmed().toStdString(),
                               tags.trimmed().toStdString(),
                               publicListed);
    refreshGroupList();
  }
  if (publicListed) {
    showToast(QStringLiteral(
        "Публичный режим сохранён локально. Поиск на rendezvous — в будущих версиях."));
  }
}

void NodeController::updateGroupMeta(const QString& groupIdHex,
                                     const QString& description,
                                     const QString& direction,
                                     const QString& tags,
                                     bool publicListed) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (gid.size() != 64) {
    showToast(QStringLiteral("Неверный id поля"), true);
    return;
  }
  bool is_owner = false;
  if (field_info_open_ && field_info_group_id_.trimmed().toLower() == gid) {
    is_owner = field_info_is_owner_;
  } else {
    for (const auto& item : group_list_) {
      const QVariantMap m = item.toMap();
      if (m.value(QStringLiteral("groupId")).toString().trimmed().toLower() != gid)
        continue;
      is_owner = m.value(QStringLiteral("isOwner")).toBool();
      break;
    }
  }
  if (!is_owner) {
    showToast(QStringLiteral("Мету поля может менять только создатель"), true);
    return;
  }
  if (!service_.update_group_meta(gid.toStdString(),
                                  description.trimmed().toStdString(),
                                  direction.trimmed().toStdString(),
                                  tags.trimmed().toStdString(),
                                  publicListed)) {
    showToast(QStringLiteral("Не удалось сохранить мету поля"), true);
    return;
  }
  refreshGroupList();
  if (field_info_open_)
    syncFieldInfoState();
  emit fieldInfoOpenChanged();
  showToast(QStringLiteral("Мете поля обновлена"));
}

void NodeController::removeConversation(const QString& key) {
  const QString sid = key.trimmed();
  if (sid.isEmpty()) {
    showToast(QStringLiteral("Чат не выбран"));
    return;
  }

  const bool was_active =
      (active_chat_key_ == sid) ||
      (sid.startsWith(QStringLiteral("group:")) &&
       active_chat_ref_id_.trimmed().toLower() == sid.mid(6).trimmed().toLower()) ||
      (sid.startsWith(QStringLiteral("dm:")) &&
       active_chat_ref_id_.trimmed().toLower() == sid.mid(3).trimmed().toLower());

  if (!service_.remove_conversation(sid.toStdString())) {
    showToast(QStringLiteral("Не удалось удалить"), true);
    return;
  }

  if (was_active) {
    endLiveSession();
    peer_title_.clear();
    active_chat_key_.clear();
    active_chat_ref_id_.clear();
    active_chat_kind_ = 0;
    messages_.clear();
    emit chatChanged();
  }

  refreshGroupList();
  refreshContactList();
  refreshChatList();
  showToast(sid.startsWith(QStringLiteral("group:")) ? QStringLiteral("Поле удалено из списка")
                                                     : QStringLiteral("Чат удалён из списка"));
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

void NodeController::setFieldMemberRole(const QString& groupIdHex,
                                        const QString& userIdHex,
                                        const QString& role) {
  const QString gid = groupIdHex.trimmed().toLower();
  const QString uid = userIdHex.trimmed().toLower();
  const QString r = role.trimmed().toLower();
  if (gid.size() != 64 || uid.size() != 64 ||
      (r != QLatin1String("host") && r != QLatin1String("member"))) {
    showToast(QStringLiteral("Неверные параметры роли"), true);
    return;
  }
  if (!service_.set_field_member_role(gid.toStdString(), uid.toStdString(), r.toStdString())) {
    showToast(QStringLiteral("Не удалось назначить роль (нужен эфир владельца)"), true);
    return;
  }
  refreshGroupList();
  refreshFieldRoster();
  showToast(r == QLatin1String("host") ? QStringLiteral("Назначен ведущий звонков")
                                       : QStringLiteral("Роль: участник"));
}

void NodeController::startFieldHub(const QString& groupIdHex) {
  const QString gid = groupIdHex.trimmed().toLower();
  if (gid.size() != 64) {
    showToast(QStringLiteral("Неверный id поля"));
    return;
  }

  if (active_chat_key_ != QStringLiteral("group:") + gid || !in_chat_) {
    showGroupInView(gid);
  }
  peer_status_text_ = QStringLiteral("открытие эфира…");
  emit chatChanged();
  showToast(QStringLiteral("Открываем эфир…"));
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
  pending_field_join_notify_ = true;

  nyx::InviteToken token {};
  if (nyx::GroupStore::invite_from_hex(normalized.toStdString(), token)) {
    nyx::GroupStore store;
    store.load();
    for (const auto& gr : store.all()) {
      if (gr.invite_token != token)
        continue;
      const std::string key = nyx_app::make_group_session_id(nyx::GroupStore::group_id_hex(gr.id));
      nyx::SessionIntent intent;
      intent.key = key;
      intent.kind = nyx::SessionIntentKind::GroupJoin;
      intent.ref_id_hex = nyx::GroupStore::group_id_hex(gr.id);
      intent.invite_hex = normalized.toStdString();
      intent.enabled = true;
      service_.enable_session_intent(std::move(intent));
      service_.reset_join_reconnect_budget(key);
      chat_list_.setSessionState(QString::fromStdString(key), QStringLiteral("connecting"));
      break;
    }
  }

  showToast(QStringLiteral("Подключение к полю…"));
  service_.start_group_join(normalized.toStdString(), false);
  emit listeningChanged();
  emit busyChanged();
  emit sessionsChanged();
  QTimer::singleShot(0, this, [this]() { refreshChatList(); });
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

  bool is_owner = false;
  QString invite;
  for (const QVariant& v : group_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("groupId")).toString() != gid)
      continue;
    is_owner = m.value(QStringLiteral("isOwner")).toBool();
    invite = m.value(QStringLiteral("invite")).toString();
    break;
  }

  if (invite.isEmpty()) {
    nyx::GroupStore store;
    store.load();
    nyx::GroupId group_id {};
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
  if (text.isEmpty())
    return;
  QGuiApplication::clipboard()->setText(text);
  toast_is_error_ = false;
  toast_ = QStringLiteral("Скопировано");
  emit toastChanged();
}

void NodeController::clearToast() {
  if (toast_.isEmpty())
    return;
  toast_.clear();
  toast_is_error_ = false;
  emit toastChanged();
}

void NodeController::setNativeChromeDark(bool dark) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  if (auto* hints = QGuiApplication::styleHints())
    hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif
  nyxApplyNativeChromeDarkAll(dark);
}

void NodeController::setWindowActive(bool active) {
  if (window_active_ == active)
    return;
  window_active_ = active;
  emit windowActiveChanged();
  syncCallNotifications();
}

void NodeController::syncCallNotifications() {
  const auto st = service_.call_state();
  const QString title = callTitle();
  const bool video = callVideo();
  const QString cid = QString::fromStdString(service_.call_id_hex());

  QString key;
  switch (st) {
  case nyx::CallState::Incoming:

    key = QStringLiteral("incoming:%1:%2").arg(title, cid);
    break;
  case nyx::CallState::Active:
    key = QStringLiteral("active:%1:%2:%3").arg(title).arg(video ? 1 : 0).arg(cid);
    break;
  case nyx::CallState::Outgoing:
  case nyx::CallState::Ringing:
    key = QStringLiteral("outgoing:%1:%2").arg(title, cid);
    break;
  default:
    key = QStringLiteral("idle");
    break;
  }
  if (key == last_call_notify_key_)
    return;
  last_call_notify_key_ = key;

#if defined(Q_OS_ANDROID)
  if (st == nyx::CallState::Incoming && answering_call_) {
    nyx_android::stop_ringtone();
    return;
  }
  if (st != nyx::CallState::Incoming)
    answering_call_ = false;
  if (st == nyx::CallState::Incoming) {
    nyx_android::cancel_call_notifications();
    nyx_android::acquire_call_wake_lock();
    nyx_android::show_incoming_call_notification(title.toStdString());
    nyx_android::bring_app_to_foreground();
  } else if (st == nyx::CallState::Active || st == nyx::CallState::Outgoing ||
             st == nyx::CallState::Ringing) {
    if (st == nyx::CallState::Active)
      nyx_android::show_active_call_notification(title.toStdString(), video);
  } else {
    nyx_android::cancel_call_notifications();
    nyx_android::release_call_wake_lock();
  }
#else
  if (st == nyx::CallState::Incoming && tray_icon_ && !window_active_) {
    tray_icon_->showMessage(QStringLiteral("Входящий звонок"),
                            title.isEmpty() ? QStringLiteral("Nyx") : title,
                            QSystemTrayIcon::Information,
                            8000);
  }
#endif
}
