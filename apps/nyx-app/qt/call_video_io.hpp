#pragma once

/** Видеозвонок: Qt Multimedia → JPEG-кадры (фрагментация realtime). */

#include <QObject>
#include <QImage>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;
class QTimer;
class QCameraDevice;

namespace nyx {
class CallVideoReassembler;
}

class CallVideoIo : public QObject {
  Q_OBJECT
 public:
  using SendFn = std::function<bool(const QByteArray& frag)>;

  explicit CallVideoIo(QObject* parent = nullptr);
  ~CallVideoIo() override;

  void setSendFn(SendFn fn) { send_fn_ = std::move(fn); }
  bool start();
  void stop();
  bool running() const { return running_; }
  bool capturing() const { return capturing_; }

  QImage lastRemoteFrame() const { return remote_; }
  QImage lastLocalFrame() const { return local_; }
  QString focusedPeerId() const { return focused_peer_; }
  void setFocusedPeerId(const QString& peerId);
  QStringList videoPeerIds() const;

  bool canSwitchCamera() const;
  bool switchCamera();

  QString preferredCameraId() const { return preferred_camera_id_; }
  void setPreferredCameraId(const QString& id);
  QString activeCameraId() const { return camera_id_; }
  static QVariantList listCameraDevices();

 signals:
  void remoteFrameChanged(const QString& peerId);
  void localFrameChanged();
  void videoPeersChanged();
  void cameraChanged();

 public slots:
  void onRemoteVideo(const QString& peerId, const QByteArray& frag_payload);

 private slots:
  void onVideoFrame();
  void onEncodeTick();

 private:
  struct PeerDecoder {
    std::unique_ptr<nyx::CallVideoReassembler> reasm;
    QImage frame;
  };

  bool openCamera(const QCameraDevice& device);
  QCameraDevice resolveCameraDevice() const;
  PeerDecoder& peerDecoder(const QString& peerId);

  SendFn send_fn_;
  std::unique_ptr<QCamera> camera_;
  std::unique_ptr<QMediaCaptureSession> session_;
  QVideoSink* sink_ = nullptr;
  QTimer* encode_timer_ = nullptr;
  QImage pending_;
  QImage local_;
  QImage remote_;
  QString focused_peer_;
  QString camera_id_;
  QString preferred_camera_id_;
  int camera_index_ = 0;
  bool front_camera_ = false;
  bool running_ = false;
  bool capturing_ = false;
  uint16_t frame_id_ = 0;

  std::unordered_map<std::string, PeerDecoder> peers_;
};
