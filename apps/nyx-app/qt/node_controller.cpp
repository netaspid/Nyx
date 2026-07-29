#include "node_controller.hpp"
#include "android_platform.hpp"
#include "document_viewer.hpp"
#include "host_env.hpp"
#include "node_controller_media.hpp"
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

} // namespace

#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace {

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
  if (file_index_thread_.joinable())
    file_index_thread_.join();
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
        const QString name = nyxSafeMediaPathPart(QString::fromStdString(block.caption));
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
        const QString relative_dir = nyxMediaRelativeDir(chat_key, title, kind);
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
          ? nyxMediaRelativeDir(chat_key, peer_title_, mediaKind)
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
