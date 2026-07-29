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

QIcon makeTrayIcon() {
  return nyxAppIcon();
}

} // namespace

NodeController::NodeController(QObject* parent) : QObject(parent), call_ui_(this), files_ui_(this) {
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

  call_ui_.call_audio_thread_.setObjectName(QStringLiteral("nyx-call-audio"));
  call_ui_.call_audio_.moveToThread(&call_ui_.call_audio_thread_);
  connect(&call_ui_.call_audio_, &CallAudioIo::startFailed, this, [this]() {
    showToast(QStringLiteral("Микрофон/динамик недоступны — только сигналинг"), true);
  });
  connect(&call_ui_.call_audio_,
          &CallAudioIo::micLevelChanged,
          this,
          &NodeController::audioTestLevelChanged);
  connect(
      &call_ui_.call_audio_, &CallAudioIo::micTestChanged, this, &NodeController::audioTestChanged);
  connect(&call_ui_.call_audio_, &CallAudioIo::localVoiceActiveChanged, this, [this](bool active) {
    const bool small_field =
        service_.call_is_field_room() && service_.call_participants().size() <= 2;
    call_ui_.call_video_.setTransmitEnabled(!service_.call_is_field_room() || small_field ||
                                            active);
  });
  connect(&call_ui_.call_audio_,
          &CallAudioIo::dominantSpeakerChanged,
          this,
          [this](const QString& peerId) {
            if (!service_.call_is_field_room())
              return;
            if (QDateTime::currentMSecsSinceEpoch() < call_ui_.manual_call_focus_until_ms_)
              return;
            call_ui_.manual_call_focus_.clear();
            call_ui_.call_video_.setFocusedPeerId(peerId);
            if (peerId.isEmpty()) {
              if (call_ui_.call_frames_)
                call_ui_.call_frames_->setPrimaryRemoteKey(QString());
              call_ui_.call_remote_frame_url_.clear();
              emit callRemoteFrameChanged();
            }
            emit callVideoPeersChanged();
          });
  call_ui_.call_audio_thread_.start();

  call_ui_.call_video_thread_.setObjectName(QStringLiteral("nyx-call-video"));
  call_ui_.call_video_.moveToThread(&call_ui_.call_video_thread_);
  call_ui_.call_video_thread_.start();

#if defined(Q_OS_ANDROID)
  connect(
      qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden ||
            state == Qt::ApplicationInactive) {
          if (service_.call_state() == nyx::CallState::Active &&
              service_.call_mode() == nyx::CallMode::AudioVideo && service_.call_camera_on()) {
            call_ui_.resume_call_camera_ = true;
            call_ui_.suspended_call_id_ = QString::fromStdString(service_.call_id_hex());
            call_ui_.call_video_.setCameraEnabled(false);
          }
          return;
        }
        if (state != Qt::ApplicationActive || !call_ui_.resume_call_camera_)
          return;
        const QString current = QString::fromStdString(service_.call_id_hex());
        const bool restore = service_.call_state() == nyx::CallState::Active &&
                             current == call_ui_.suspended_call_id_ && service_.call_camera_on();
        call_ui_.resume_call_camera_ = false;
        call_ui_.suspended_call_id_.clear();
        if (restore) {
          QTimer::singleShot(150, this, [this]() {
            if (service_.call_state() != nyx::CallState::Active || !service_.call_camera_on()) {
              return;
            }
            call_ui_.call_video_.setCameraEnabled(true);
            call_ui_.call_video_.start();
            emit callChanged();
          });
        }
      });
#endif

  connect(&call_ui_.call_video_, &CallVideoIo::cameraOpenFailed, this, [this]() {
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
    files_ui_.file_scope_group_id_ = QString::fromStdString(saved_scope).trimmed().toLower();
  }
  const std::string saved_root = service_.load_files_selected_root();
  if (!saved_root.empty()) {
    files_ui_.file_selected_share_root_ = QString::fromStdString(saved_root);
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
  if (files_ui_.file_index_thread_.joinable())
    files_ui_.file_index_thread_.join();
#if defined(Q_OS_ANDROID)
  nyx_android::stop_keepalive_service();
  nyx_android::cancel_call_notifications();
#endif
  if (tray_icon_) {
    tray_icon_->hide();
  }
  if (call_ui_.call_audio_thread_.isRunning()) {
    QMetaObject::invokeMethod(
        &call_ui_.call_audio_,
        [this]() {
          call_ui_.call_audio_.stop();
          call_ui_.call_audio_thread_.quit();
        },
        Qt::QueuedConnection);
    if (!call_ui_.call_audio_thread_.wait(5000))
      call_ui_.call_audio_thread_.wait();
  } else {
    call_ui_.call_audio_.stop();
  }
  if (call_ui_.call_video_thread_.isRunning()) {
    QMetaObject::invokeMethod(
        &call_ui_.call_video_,
        [this]() {
          call_ui_.call_video_.stop();
          call_ui_.call_video_thread_.quit();
        },
        Qt::QueuedConnection);
    if (!call_ui_.call_video_thread_.wait(5000))
      call_ui_.call_video_thread_.wait();
  } else {
    call_ui_.call_video_.stop();
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
  if (key == call_ui_.last_call_notify_key_)
    return;
  call_ui_.last_call_notify_key_ = key;

#if defined(Q_OS_ANDROID)
  if (st == nyx::CallState::Incoming && call_ui_.answering_call_) {
    nyx_android::stop_ringtone();
    return;
  }
  if (st != nyx::CallState::Incoming)
    call_ui_.answering_call_ = false;
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
