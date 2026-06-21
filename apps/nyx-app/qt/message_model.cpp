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
    default:
      return {};
  }
}

QHash<int, QByteArray> MessageModel::roleNames() const {
  return {{AuthorRole, "author"},
          {MessageTextRole, "messageText"},
          {OutgoingRole, "outgoing"},
          {TimestampRole, "timestamp"}};
}

void MessageModel::clear() {
  beginResetModel();
  rows_.clear();
  all_rows_.clear();
  filter_.clear();
  endResetModel();
}

void MessageModel::appendMessage(const QString& author, const QString& text, bool outgoing,
                                 quint64 timestampMs) {
  const Row row{author, text, outgoing, timestampMs};
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

void MessageModel::setFilter(const QString& query) {
  filter_ = query.trimmed();
  beginResetModel();
  rows_.clear();
  for (const Row& row : all_rows_) {
    if (!filter_.isEmpty() &&
        !row.text.contains(filter_, Qt::CaseInsensitive) &&
        !row.author.contains(filter_, Qt::CaseInsensitive)) {
      continue;
    }
    rows_.push_back(row);
  }
  endResetModel();
}
