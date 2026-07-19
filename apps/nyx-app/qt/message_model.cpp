#include "message_model.hpp"

MessageModel::MessageModel(QObject* parent) : QAbstractListModel(parent) {}

int MessageModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return rows_.size();
}

QVariant MessageModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
  const Row& row = rows_.at(index.row());
  switch (role) {
    case AuthorRole:
      return row.author;
    case MessageTextRole:
      return row.text;
    case OutgoingRole:
      return row.outgoing;
    case TimestampRole:
      return row.timestamp;
    case MessageIdRole:
      return QVariant::fromValue(row.message_id);
    case DeliveryRole:
      return row.delivery;
    case AuthorUserIdRole:
      return row.author_user_id;
    default:
      return {};
  }
}

QHash<int, QByteArray> MessageModel::roleNames() const {
  return {{AuthorRole, "author"},
          {MessageTextRole, "messageText"},
          {OutgoingRole, "outgoing"},
          {TimestampRole, "timestamp"},
          {MessageIdRole, "messageId"},
          {DeliveryRole, "delivery"},
          {AuthorUserIdRole, "authorUserId"}};
}

void MessageModel::clear() {
  beginResetModel();
  rows_.clear();
  all_rows_.clear();
  filter_.clear();
  endResetModel();
}

void MessageModel::appendMessage(const QString& author, const QString& text, bool outgoing,
                                 quint64 timestampMs, quint64 messageId,
                                 const QString& delivery, const QString& authorUserId) {
  const Row row{author, text, outgoing, timestampMs, messageId, delivery, authorUserId};
  all_rows_.push_back(row);
  if (!filter_.isEmpty() &&
      !text.contains(filter_, Qt::CaseInsensitive) &&
      !author.contains(filter_, Qt::CaseInsensitive)) {
    return;
  }
  const int r = rows_.size();
  beginInsertRows(QModelIndex(), r, r);
  rows_.push_back(row);
  endInsertRows();
}

void MessageModel::setDelivery(quint64 messageId, const QString& delivery) {
  if (messageId == 0) return;
  for (int i = 0; i < all_rows_.size(); ++i) {
    if (all_rows_[i].message_id != messageId) continue;
    if (all_rows_[i].delivery == delivery) return;
    all_rows_[i].delivery = delivery;
    break;
  }
  for (int i = 0; i < rows_.size(); ++i) {
    if (rows_[i].message_id != messageId) continue;
    if (rows_[i].delivery == delivery) return;
    rows_[i].delivery = delivery;
    const QModelIndex idx = index(i);
    emit dataChanged(idx, idx, {DeliveryRole});
    return;
  }
}

bool MessageModel::hasMessageId(quint64 messageId) const {
  if (messageId == 0) return false;
  for (const Row& row : all_rows_) {
    if (row.message_id == messageId) return true;
  }
  return false;
}

void MessageModel::setFilter(const QString& query) {
  filter_ = query.trimmed();
  beginResetModel();
  rows_.clear();
  applyFilter();
  endResetModel();
}

void MessageModel::applyFilter() {
  for (const Row& row : all_rows_) {
    if (!filter_.isEmpty() &&
        !row.text.contains(filter_, Qt::CaseInsensitive) &&
        !row.author.contains(filter_, Qt::CaseInsensitive)) {
      continue;
    }
    rows_.push_back(row);
  }
}
