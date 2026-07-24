#pragma once

/** Video call: capture → AV1 + parity-protected realtime fragments. */

#include <QObject>
#include <QImage>
#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;
class QTimer;
class QCameraDevice;
class QVideoFrame;

namespace nyx {
class CallVideoReassembler;
class Av1Encoder;
class Av1Decoder;
}

class CallVideoIo : public QObject {
  Q_OBJECT
 public:
  using SendFn = std::function<bool(const QByteArray& frag)>;

  explicit CallVideoIo(QObject* parent = nullptr);
  ~CallVideoIo() override;

  void setSendFn(SendFn fn);
  Q_INVOKABLE bool start();
  Q_INVOKABLE void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  bool capturing() const { return capturing_.load(std::memory_order_acquire); }

  QImage lastRemoteFrame() const;
  QImage lastLocalFrame() const;
  QImage peerFrame(const QString& peerId) const;
  QString focusedPeerId() const;
  void setFocusedPeerId(const QString& peerId);
  QStringList videoPeerIds() const;

  bool canSwitchCamera() const;
  Q_INVOKABLE bool switchCamera();

  Q_INVOKABLE void setCameraEnabled(bool on);
  bool cameraEnabled() const { return camera_enabled_.load(std::memory_order_acquire); }
  void setTransmitEnabled(bool on) {
    const bool previous = transmit_enabled_.exchange(on, std::memory_order_acq_rel);
    if (on && !previous) force_keyframe_.store(true, std::memory_order_release);
  }

  QString preferredCameraId() const;
  void setPreferredCameraId(const QString& id);
  QString activeCameraId() const;
  static QVariantList listCameraDevices();

 signals:
  void remoteFrameChanged(const QString& peerId);
  void localFrameChanged();
  void videoPeersChanged();
  void cameraChanged();
  void cameraOpenFailed();

 public slots:
  void onRemoteVideo(const QString& peerId, const QByteArray& frag_payload);

 private slots:
  void onEncodeTick();
  void ingestCapturedFrame(QImage cropped);
#if defined(Q_OS_ANDROID)
  void onNativeJpeg(const QByteArray& jpeg, bool front);
  void onNativeCameraError(const QString& message);
  void onNativeCameraStarted(bool front, const QString& cameraId);
#endif

 private:
  struct PeerDecoder {
    std::unique_ptr<nyx::CallVideoReassembler> reasm;
    std::unique_ptr<nyx::Av1Decoder> decoder;
    QImage frame;
  };

  bool openCamera(const QCameraDevice& device);
  void closeCameraHardware();
  void wireVideoSink();
  void handleCameraFrame(const QVideoFrame& frame);
  QCameraDevice resolveCameraDevice() const;
  PeerDecoder& peerDecoder(const QString& peerId);
  bool ensureOnVideoThread(const char* where);
  int encodeWidth() const;
  int encodeHeight() const;
  int encodeFps() const;

  SendFn send_fn_;
  std::unique_ptr<nyx::Av1Encoder> encoder_;
#if !defined(Q_OS_ANDROID)
  std::unique_ptr<QCamera> camera_;
  std::unique_ptr<QMediaCaptureSession> session_;
  QVideoSink* sink_ = nullptr;
  bool sink_wired_ = false;
#endif
  QTimer* encode_timer_ = nullptr;
  mutable QMutex frames_mutex_;
  QImage pending_;
  QImage local_;
  QImage remote_;
  QString focused_peer_;
  QString camera_id_;
  QString preferred_camera_id_;
  int camera_index_ = 0;
  std::atomic<bool> front_camera_{true};
  std::atomic<bool> running_{false};
  std::atomic<bool> capturing_{false};
  std::atomic<bool> camera_enabled_{true};
  std::atomic<bool> transmit_enabled_{true};
  std::atomic<bool> force_keyframe_{true};
  std::atomic<bool> ingest_busy_{false};
  std::atomic<qint64> last_ingest_ms_{0};
  bool local_dirty_ = false;
  uint16_t frame_id_ = 0;
  bool encode_busy_ = false;

  std::unordered_map<std::string, PeerDecoder> peers_;
};
