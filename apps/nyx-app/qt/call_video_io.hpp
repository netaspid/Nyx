#pragma once

/** Видеозвонок: Qt Multimedia → AV1 (libaom) с фрагментацией realtime. */

#include <QObject>
#include <QImage>
#include <QByteArray>

#include <cstdint>
#include <functional>
#include <memory>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;
class QTimer;

namespace nyx {
class Av1Encoder;
class Av1Decoder;
class CallVideoReassembler;
}

class CallVideoIo : public QObject {
  Q_OBJECT
 public:
  using SendFn = std::function<bool(const QByteArray& av1_frag)>;

  explicit CallVideoIo(QObject* parent = nullptr);
  ~CallVideoIo() override;

  void setSendFn(SendFn fn) { send_fn_ = std::move(fn); }
  bool start();
  void stop();
  bool running() const { return running_; }
  QImage lastRemoteFrame() const { return remote_; }

 signals:
  void remoteFrameChanged();
  void localFrameChanged(const QImage& frame);

 public slots:
  void onRemoteVideo(const QByteArray& frag_payload);

 private slots:
  void onVideoFrame();
  void onEncodeTick();

 private:
  SendFn send_fn_;
  std::unique_ptr<QCamera> camera_;
  std::unique_ptr<QMediaCaptureSession> session_;
  QVideoSink* sink_ = nullptr;
  QTimer* encode_timer_ = nullptr;
  QImage pending_;
  QImage remote_;
  bool running_ = false;
  uint16_t frame_id_ = 0;
  int keyframe_counter_ = 0;

  std::unique_ptr<nyx::Av1Encoder> encoder_;
  std::unique_ptr<nyx::Av1Decoder> decoder_;
  std::unique_ptr<nyx::CallVideoReassembler> reasm_;
};
