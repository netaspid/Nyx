#pragma once

/** Захват/воспроизведение аудио звонка через Qt Multimedia + Opus. */

#include "nyx/call_opus.hpp"

#include <QObject>
#include <QByteArray>

#include <functional>
#include <memory>
#include <vector>

class QIODevice;
class QAudioSource;
class QAudioSink;
class QTimer;

class CallAudioIo : public QObject {
  Q_OBJECT
 public:
  using SendFn = std::function<bool(const std::vector<uint8_t>&)>;

  explicit CallAudioIo(QObject* parent = nullptr);
  ~CallAudioIo() override;

  void setSendFn(SendFn fn) { send_fn_ = std::move(fn); }
  bool start();
  void stop();
  bool running() const { return running_; }

 public slots:
  void onRemoteOpus(const QByteArray& packet);

 private slots:
  void onCaptureReady();

 private:
  SendFn send_fn_;
  nyx::OpusEncoderWrap encoder_;
  nyx::OpusDecoderWrap decoder_;
  std::unique_ptr<QAudioSource> source_;
  std::unique_ptr<QAudioSink> sink_;
  QIODevice* source_dev_ = nullptr;
  QIODevice* sink_dev_ = nullptr;
  QTimer* timer_ = nullptr;
  bool running_ = false;
  std::vector<int16_t> capture_buf_;
};
