#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>

/** Список чатов (контакты + поля) для левой колонки QML. */
class ChatListModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Roles {
    KeyRole = Qt::UserRole + 1,
    TitleRole,
    PreviewRole,
    TimestampRole,
    KindRole,
    RefIdRole,
    UnreadRole,
    LastSeenRole,
    TimeLabelRole,
  };

  explicit ChatListModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE void refreshFromDisk(const QString& selfIdHex = {});
  Q_INVOKABLE int indexForKey(const QString& key) const;
  Q_INVOKABLE void setUnread(const QString& key, int count);
  Q_INVOKABLE void bumpUnread(const QString& key);
  Q_INVOKABLE void clearUnread(const QString& key);

 private:
  struct Row {
    QString key;
    QString title;
    QString preview;
    quint64 timestamp = 0;
    int kind = 0;
    QString refId;
    int unread = 0;
    QString lastSeen;
    QString timeLabel;
  };

  QVector<Row> rows_;
  QHash<QString, int> unread_;
};
