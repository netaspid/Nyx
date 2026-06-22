#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>

/** Список файлов (локальный или удалённый) для FilesDialog. */
class FileListModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Roles {
    NameRole = Qt::UserRole + 1,
    HashRole,
    SizeRole,
    SizeLabelRole,
    MimeRole,
    IsRemoteRole,
    IsDirectoryRole,
    NavPathRole,
    RootPathRole,
  };

  explicit FileListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  void setEntries(const QVariantList& entries);

 private:
  QVariantList rows_;
};
