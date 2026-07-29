#include "node_controller.hpp"

#include "android_platform.hpp"
#include "node_controller_media.hpp"

#include "nyx/account_store.hpp"
#include "nyx/avatar_store.hpp"
#include "nyx/conversation.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/markdown_format.hpp"
#include "nyx/message_store.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/util.hpp"

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryFile>
#include <QVariantList>
#include <QVariantMap>

#include <string>

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
    const QString norm = nyxNormalizeSessionKey(key);
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
    const QString norm = nyxNormalizeSessionKey(key);
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
    if (!nyxParseUserIdHex(refId, peer))
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

  QTimer::singleShot(0, this, [this]() { files_ui_.notifyFilesChanged(); });

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
    if (files_ui_.file_scope_group_id_ != gid) {
      files_ui_.file_scope_group_id_ = gid;
      syncFileScopeLabel();
    }
  } else {
    peer_status_text_ = QStringLiteral("на связи");
    if (!refId.isEmpty())
      active_chat_key_ = QStringLiteral("dm:") + refId;
  }
  emit chatChanged();
  emit busyChanged();
  files_ui_.notifyFilesChanged();
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
  files_ui_.file_progress_visible_ = false;
  files_ui_.file_progress_percent_ = 0;
  files_ui_.file_progress_label_.clear();
  files_ui_.notifyFileProgressChanged();
  emit chatChanged();
  emit busyChanged();
  refreshChatList();
  refreshGroupList();
  refreshRemoteFileModel();
  files_ui_.notifyFilesChanged();
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
  files_ui_.file_progress_visible_ = false;
  files_ui_.file_progress_percent_ = 0;
  files_ui_.file_progress_label_.clear();
  files_ui_.notifyFileProgressChanged();
  emit chatChanged();
  emit busyChanged();
  refreshChatList();
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
