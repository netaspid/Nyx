#include "call_frame_provider.hpp"

CallFrameProvider::CallFrameProvider()
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage CallFrameProvider::requestImage(const QString& id, QSize* size,
                                       const QSize& requestedSize) {
  Q_UNUSED(requestedSize);
  const QString key = id.section(QLatin1Char('/'), 0, 0);
  QImage img;
  {
    QMutexLocker lock(&mutex_);
    if (key == QLatin1String("local")) {
      img = local_;
    } else if (key == QLatin1String("remote")) {
      const QString pk = primary_.isEmpty() ? QStringLiteral("direct") : primary_;
      img = remotes_.value(pk);
      if (img.isNull() && !remotes_.isEmpty()) img = remotes_.cbegin().value();
    } else if (key == QLatin1String("peer")) {
      img = remotes_.value(id.section(QLatin1Char('/'), 1, 1));
    }
  }
  if (size) *size = img.size();
  return img;
}

void CallFrameProvider::clear() {
  QMutexLocker lock(&mutex_);
  local_ = QImage();
  remotes_.clear();
  primary_.clear();
}

void CallFrameProvider::setLocal(const QImage& img) {
  QMutexLocker lock(&mutex_);
  local_ = img;
}

void CallFrameProvider::setRemote(const QString& peerKey, const QImage& img) {
  if (peerKey.isEmpty()) return;
  QMutexLocker lock(&mutex_);
  remotes_.insert(peerKey, img);
  if (primary_.isEmpty()) primary_ = peerKey;
}

void CallFrameProvider::removeRemote(const QString& peerKey) {
  QMutexLocker lock(&mutex_);
  remotes_.remove(peerKey);
  if (primary_ == peerKey) {
    primary_ = remotes_.isEmpty() ? QString() : remotes_.cbegin().key();
  }
}

QImage CallFrameProvider::local() const {
  QMutexLocker lock(&mutex_);
  return local_;
}

QImage CallFrameProvider::remote(const QString& peerKey) const {
  QMutexLocker lock(&mutex_);
  return remotes_.value(peerKey);
}

QString CallFrameProvider::primaryRemoteKey() const {
  QMutexLocker lock(&mutex_);
  return primary_;
}

void CallFrameProvider::setPrimaryRemoteKey(const QString& key) {
  QMutexLocker lock(&mutex_);
  if (key.isEmpty() || remotes_.contains(key)) primary_ = key;
}

QStringList CallFrameProvider::remotePeerKeys() const {
  QMutexLocker lock(&mutex_);
  return remotes_.keys();
}
