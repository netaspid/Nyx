#include "node_controller.hpp"

#include "nyx/account_store.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/conversation.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <QAction>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QVariantMap>

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

void NodeController::openGroupsDialog() {
  refreshGroupList();
  setGroupsDialogOpen(true);
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
          endLiveSession();
          refreshGroupList();
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
  if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
    peer_status_text_ = QStringLiteral("в поле");
    active_chat_key_ = QStringLiteral("group:") + refId;
  } else {
    peer_status_text_ = QStringLiteral("в сети");
    if (!refId.isEmpty()) active_chat_key_ = QStringLiteral("dm:") + refId;
  }
  emit chatChanged();
  emit busyChanged();
}

void NodeController::endLiveSession() {
  if (!in_chat_) return;
  if (service_.busy()) return;
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
  leaveChat();
  if (!service_.start_listen(true)) {
    setStatus(QStringLiteral("Не удалось начать прослушивание (завершите текущий чат)"));
    return;
  }
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
  leaveChat();
  if (!service_.start_connect_token(tokenHex.trimmed().toStdString())) {
    setStatus(QStringLiteral("Не удалось подключиться (завершите текущий чат)"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::connectPeer(const QString& host, int port) {
  leaveChat();
  if (!service_.start_connect_peer(host.toStdString(), static_cast<uint16_t>(port))) {
    setStatus(QStringLiteral("Не удалось подключиться (завершите текущий чат)"));
    return;
  }
  setConnectionPanelOpen(false);
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::disconnectSession() {
  service_.stop();
  endLiveSession();
  emit listeningChanged();
  emit busyChanged();
}

void NodeController::sendMessage(const QString& text) {
  if (text.trimmed().isEmpty() || !in_chat_) return;
  service_.send_message(text.toStdString());
}

void NodeController::indexFolder(const QString& path) {
  QString p = path;
  if (p.startsWith("file:///")) p = QUrl(p).toLocalFile();
  service_.index_folder(p.toStdString());
}

void NodeController::requestRemoteFiles() { service_.request_remote_files(); }

void NodeController::downloadFile(const QString& hashHex) {
  service_.download_file(hashHex.trimmed().toStdString());
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
