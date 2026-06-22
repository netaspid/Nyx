#include "file_list_model.hpp"

namespace {

QString formatSize(quint64 bytes) {
  if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
  if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
  if (bytes < 1024ULL * 1024 * 1024) {
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
  }
  return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 1) + QStringLiteral(" GB");
}

}  // namespace

FileListModel::FileListModel(QObject* parent) : QAbstractListModel(parent) {}

int FileListModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return rows_.size();
}

QVariant FileListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
  const QVariantMap m = rows_.at(index.row()).toMap();
  switch (role) {
    case NameRole:
      return m.value(QStringLiteral("name"));
    case HashRole:
      return m.value(QStringLiteral("hash"));
    case SizeRole:
      return m.value(QStringLiteral("size"));
    case SizeLabelRole: {
      if (m.value(QStringLiteral("isDirectory")).toBool()) {
        const quint64 n = m.value(QStringLiteral("size")).toULongLong();
        if (n == 0) return QStringLiteral("пустая папка");
        if (n == 1) return QStringLiteral("1 файл");
        return QStringLiteral("%1 файлов").arg(n);
      }
      return formatSize(m.value(QStringLiteral("size")).toULongLong());
    }
    case MimeRole:
      return m.value(QStringLiteral("mime"));
    case IsRemoteRole:
      return m.value(QStringLiteral("isRemote"));
    case IsDirectoryRole:
      return m.value(QStringLiteral("isDirectory"));
    case NavPathRole:
      return m.value(QStringLiteral("navPath"));
    case RootPathRole:
      return m.value(QStringLiteral("rootPath"));
    default:
      return {};
  }
}

QHash<int, QByteArray> FileListModel::roleNames() const {
  return {
      {NameRole, "name"},
      {HashRole, "hash"},
      {SizeRole, "size"},
      {SizeLabelRole, "sizeLabel"},
      {MimeRole, "mime"},
      {IsRemoteRole, "isRemote"},
      {IsDirectoryRole, "isDirectory"},
      {NavPathRole, "navPath"},
      {RootPathRole, "rootPath"},
  };
}

void FileListModel::setEntries(const QVariantList& entries) {
  beginResetModel();
  rows_ = entries;
  endResetModel();
}
