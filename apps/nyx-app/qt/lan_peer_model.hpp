#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QVariant>

/** Результаты LAN browse для QML. */
class LanPeerModel : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Roles {
    InstanceRole = Qt::UserRole + 1,
    HostRole,
    PortRole,
    UserIdRole,
    AddressRole,
  };

  explicit LanPeerModel(QObject* parent = nullptr);

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  Q_INVOKABLE void setPeers(const QVariantList& peers);
  Q_INVOKABLE void clear();

 private:
  struct Row {
    QString instance;
    QString host;
    int port = 0;
    QString userId;
    QString address;
  };

  QVector<Row> rows_;
};
