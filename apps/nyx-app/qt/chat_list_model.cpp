#include "chat_list_model.hpp"

#include "nyx/conversation.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"
#include "nyx/paths.hpp"

#include <QDateTime>

namespace {

QString formatListTime(quint64 ms) {
  if (ms == 0) return {};
  const QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ms));
  const QDate today = QDate::currentDate();
  if (dt.date() == today) return dt.toString(QStringLiteral("HH:mm"));
  if (dt.date().daysTo(today) == 1) return QStringLiteral("вчера");
  return dt.toString(QStringLiteral("dd.MM"));
}

}  // namespace

ChatListModel::ChatListModel(QObject* parent) : QAbstractListModel(parent) {}

int ChatListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return rows_.size();
}

QVariant ChatListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
  const Row& row = rows_.at(index.row());
  switch (role) {
    case KeyRole:
      return row.key;
    case TitleRole:
      return row.title;
    case PreviewRole:
      return row.preview;
    case TimestampRole:
      return row.timestamp;
    case KindRole:
      return row.kind;
    case RefIdRole:
      return row.refId;
    case UnreadRole:
      return row.unread;
    case LastSeenRole:
      return row.lastSeen;
    case TimeLabelRole:
      return row.timeLabel;
    default:
      return {};
  }
}

QHash<int, QByteArray> ChatListModel::roleNames() const {
  return {
      {KeyRole, "key"},
      {TitleRole, "title"},
      {PreviewRole, "preview"},
      {TimestampRole, "timestamp"},
      {KindRole, "kind"},
      {RefIdRole, "refId"},
      {UnreadRole, "unread"},
      {LastSeenRole, "lastSeen"},
      {TimeLabelRole, "timeLabel"},
  };
}

void ChatListModel::refreshFromDisk(const QString& selfIdHex) {
  (void)selfIdHex;
  nyx::Profile profile;
  if (!nyx::load_profile(nyx::default_profile_path(), profile)) {
    beginResetModel();
    rows_.clear();
    endResetModel();
    return;
  }

  const auto summaries = nyx::list_conversations(profile.user_id());
  const uint64_t now = nyx::now_ms();

  beginResetModel();
  rows_.clear();
  rows_.reserve(static_cast<int>(summaries.size()));
  for (const auto& s : summaries) {
    Row row;
    row.key = QString::fromStdString(s.key);
    row.title = QString::fromStdString(s.title);
    row.preview = QString::fromStdString(s.preview);
    row.timestamp = s.timestamp_ms;
    row.kind = static_cast<int>(s.kind);
    row.refId = s.kind == nyx::ConversationKind::Group
                    ? QString::fromStdString(s.group_id_hex)
                    : QString::fromStdString(s.peer_id_hex);
    row.unread = unread_.value(row.key, 0);
    row.lastSeen = QString::fromStdString(nyx::format_last_seen(s.last_seen_ms, now));
    row.timeLabel = formatListTime(row.timestamp);
    rows_.append(std::move(row));
  }
  endResetModel();
}

int ChatListModel::indexForKey(const QString& key) const {
  for (int i = 0; i < rows_.size(); ++i) {
    if (rows_.at(i).key == key) return i;
  }
  return -1;
}

void ChatListModel::setUnread(const QString& key, int count) {
  unread_[key] = count;
  const int idx = indexForKey(key);
  if (idx >= 0) {
    rows_[idx].unread = count;
    emit dataChanged(index(idx), index(idx), {UnreadRole});
  }
}

void ChatListModel::bumpUnread(const QString& key) {
  setUnread(key, unread_.value(key, 0) + 1);
}

void ChatListModel::clearUnread(const QString& key) {
  setUnread(key, 0);
}
