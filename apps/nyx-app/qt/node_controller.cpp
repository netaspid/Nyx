#include "node_controller.hpp"

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
    tray_menu_ = new QMenu();
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
  refreshProfile();
  rendezvous_list_ = QString::fromStdString(service_.rendezvous_list_string());
  if (rendezvous_list_.isEmpty()) rendezvous_list_ = rendezvous_;
  discovery_mode_ = static_cast<int>(service_.network_config().mode);
  emit networkSettingsChanged();
  refreshChatList();

  lan_discovery_timer_.setInterval(5000);
  connect(&lan_discovery_timer_, &QTimer::timeout, this, &NodeController::tickLanDiscovery);
  lan_discovery_timer_.start();
  QTimer::singleShot(800, this, &NodeController::tickLanDiscovery);
}

NodeController::~NodeController() { service_.stop(); }

void NodeController::setRendezvous(const QString& v) {
  if (rendezvous_ == v) return;
  rendezvous_ = v;
  rendezvous_list_ = v;
  service_.set_rendezvous(v.toStdString());
  emit rendezvousChanged();
}

void NodeController::setRendezvousList(const QString& v) {
  if (rendezvous_list_ == v) return;
  rendezvous_list_ = v;
  service_.set_rendezvous_list(v.toStdString());
  rendezvous_ = QString::fromStdString(service_.rendezvous_list_string());
  emit rendezvousChanged();
}

void NodeController::setDiscoveryMode(int mode) {
  if (discovery_mode_ == mode) return;
  discovery_mode_ = mode;
  service_.set_discovery_mode(mode);
  emit networkSettingsChanged();
}

void NodeController::saveNetworkSettings() {
  service_.save_network_config();
  network_status_ = QStringLiteral("Сохранено");
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

void NodeController::wireCallbacks() {
  service_.set_rendezvous(rendezvous_.toStdString());

  service_.set_on_status([this](const std::string& text) {
    QMetaObject::invokeMethod(
        this, [this, text]() { setStatus(QString::fromStdString(text)); },
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
            if (!in_chat_) {
              refreshChatList();
            }
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

  service_.set_on_chat_ready([this](const std::string& peer_title, const std::string& conn_label) {
    QMetaObject::invokeMethod(
        this,
        [this, peer_title, conn_label]() {
          messages_.clear();
          enterChat(QString::fromStdString(peer_title),
                    QString::fromStdString(conn_label));
          refreshChatList();
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
          emit busyChanged();
        },
        Qt::QueuedConnection);
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

void NodeController::updateOnboardingFlag() {
  QSettings settings;
  needs_onboarding_ = !settings.value(QStringLiteral("onboardingComplete"), false).toBool();
}

void NodeController::refreshProfile() {
  const auto profile = service_.profile();
  profile_nickname_ = QString::fromStdString(profile.nickname);
  profile_id_short_ = QString::fromStdString(nyx::short_user_id(profile.user_id()));
  QSettings settings;
  if (!settings.value(QStringLiteral("onboardingComplete"), false).toBool() &&
      !profile_nickname_.trimmed().isEmpty()) {
    settings.setValue(QStringLiteral("onboardingComplete"), true);
  }
  updateOnboardingFlag();
  emit profileChanged();
}

void NodeController::completeOnboarding(const QString& nickname) {
  if (nickname.trimmed().isEmpty()) return;
  setNickname(nickname);
  QSettings settings;
  settings.setValue(QStringLiteral("onboardingComplete"), true);
  updateOnboardingFlag();
  emit profileChanged();
}

void NodeController::refreshChatList() {
  chat_list_.refreshFromDisk(profile_id_short_);
}

void NodeController::loadStoredHistory(int kind, const QString& refId) {
  messages_.clear();
  const auto profile = service_.profile();
  std::string path;
  if (kind == static_cast<int>(nyx::ConversationKind::Group)) {
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
  if (in_chat_) return;
  active_chat_key_ = key;
  peer_title_ = title;
  peer_connection_label_.clear();
  peer_status_text_ = lastSeen.isEmpty() ? QStringLiteral("история") : lastSeen;
  loadStoredHistory(kind, refId);
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

void NodeController::enterChat(const QString& peerName, const QString& connectionLabel) {
  in_chat_ = true;
  peer_title_ = peerName;
  peer_connection_label_ = connectionLabel;
  peer_status_text_ = QStringLiteral("в сети");
  emit chatChanged();
  emit busyChanged();
}

void NodeController::leaveChat() {
  in_chat_ = false;
  peer_title_.clear();
  peer_connection_label_.clear();
  peer_status_text_.clear();
  active_chat_key_.clear();
  messages_.clear();
  file_progress_visible_ = false;
  file_progress_percent_ = 0;
  file_progress_label_.clear();
  emit fileProgressChanged();
  emit chatChanged();
  emit busyChanged();
  refreshChatList();
}

void NodeController::startListen() {
  leaveChat();
  service_.start_listen(true);
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
  service_.start_connect_token(tokenHex.trimmed().toStdString());
  emit busyChanged();
}

void NodeController::connectPeer(const QString& host, int port) {
  leaveChat();
  service_.start_connect_peer(host.toStdString(), static_cast<uint16_t>(port));
  emit busyChanged();
}

void NodeController::disconnectSession() {
  service_.stop();
  leaveChat();
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

void NodeController::startGroupHub(const QString& groupIdHex) {
  leaveChat();
  service_.start_group_hub(groupIdHex.trimmed().toStdString());
  emit busyChanged();
}

void NodeController::joinGroup(const QString& inviteHex) {
  leaveChat();
  service_.start_group_join(inviteHex.trimmed().toStdString());
  emit busyChanged();
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
