#pragma once

/** Image provider for call video frames (avoids JPEG temp-file bridge). */

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

class CallFrameProvider : public QQuickImageProvider {
 public:
  CallFrameProvider();

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

  void clear();
  void setLocal(const QImage& img);
  void setRemote(const QString& peerKey, const QImage& img);
  void removeRemote(const QString& peerKey);

  QImage local() const;
  QImage remote(const QString& peerKey) const;
  QString primaryRemoteKey() const;
  void setPrimaryRemoteKey(const QString& key);
  QStringList remotePeerKeys() const;

 private:
  mutable QMutex mutex_;
  QImage local_;
  QHash<QString, QImage> remotes_;
  QString primary_;
};
