#include "node_controller.hpp"

#include "nyx/account_store.hpp"
#include "nyx/group.hpp"
#include "nyx/identity.hpp"
#include "nyx/util.hpp"

#include <QVariantList>
#include <QVariantMap>

#include <string>

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
