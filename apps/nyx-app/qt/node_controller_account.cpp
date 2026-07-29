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
#include <QFileDialog>
#include <QGuiApplication>
#include <QImage>
#include <QMimeDatabase>
#include <QStandardPaths>
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
