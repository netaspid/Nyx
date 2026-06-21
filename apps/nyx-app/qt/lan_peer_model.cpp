#include "lan_peer_model.hpp"

#include <QVariantMap>

LanPeerModel::LanPeerModel(QObject* parent) : QAbstractListModel(parent) {}

int LanPeerModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) return 0;
  return rows_.size();
}

QVariant LanPeerModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return {};
  const Row& row = rows_.at(index.row());
  switch (role) {
    case InstanceRole:
      return row.instance;
    case HostRole:
      return row.host;
    case PortRole:
      return row.port;
    case UserIdRole:
      return row.userId;
    case AddressRole:
      return row.address;
    default:
      return {};
  }
}

QHash<int, QByteArray> LanPeerModel::roleNames() const {
  return {{InstanceRole, "instance"},
          {HostRole, "host"},
          {PortRole, "port"},
          {UserIdRole, "userId"},
          {AddressRole, "address"}};
}

void LanPeerModel::clear() {
  beginResetModel();
  rows_.clear();
  endResetModel();
}

void LanPeerModel::setPeers(const QVariantList& peers) {
  beginResetModel();
  rows_.clear();
  for (const QVariant& v : peers) {
    const QVariantMap m = v.toMap();
    Row row;
    row.instance = m.value("instance").toString();
    row.host = m.value("host").toString();
    row.port = m.value("port").toInt();
    row.userId = m.value("userId").toString();
    row.address = row.host + ':' + QString::number(row.port);
    rows_.push_back(std::move(row));
  }
  endResetModel();
}
