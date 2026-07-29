#include "node_controller.hpp"

#include "android_platform.hpp"
#include "host_env.hpp"

#include "nyx/account_store.hpp"
#include "nyx/avatar_store.hpp"
#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/profile_crypto.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/util.hpp"

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QImage>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
