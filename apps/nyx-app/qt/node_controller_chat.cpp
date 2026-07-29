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
