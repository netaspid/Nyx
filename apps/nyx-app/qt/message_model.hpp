#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariant>
#include <QVector>

/** Список сообщений чата для QML ListView. */
class MessageModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Roles {
    AuthorRole = Qt::UserRole + 1,
    MessageTextRole,
    OutgoingRole,
    TimestampRole,
    MessageIdRole,
    DeliveryRole,
    AuthorUserIdRole,
  };

  /** pending | delivered | failed | пусто (входящие / старая история). */
  explicit MessageModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE void clear();
  Q_INVOKABLE void appendMessage(const QString& author, const QString& text, bool outgoing,
                                 quint64 timestampMs, quint64 messageId = 0,
                                 const QString& delivery = QString(),
                                 const QString& authorUserId = QString());
  Q_INVOKABLE void setDelivery(quint64 messageId, const QString& delivery);
  Q_INVOKABLE bool hasMessageId(quint64 messageId) const;
  Q_INVOKABLE void setFilter(const QString& query);

 private:
  struct Row {
    QString author;
    QString text;
    bool outgoing = false;
    quint64 timestamp = 0;
    quint64 message_id = 0;
    QString delivery;
    QString author_user_id;
  };

  void applyFilter();

  QVector<Row> rows_;
  QVector<Row> all_rows_;
  QString filter_;
};
