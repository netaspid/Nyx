#pragma once

/** Захват/воспроизведение аудио звонка через Qt Multimedia + Opus. */

#include "nyx/call_opus.hpp"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVariantList>

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

  QString preferredInputId() const { return preferred_input_id_; }
  QString preferredOutputId() const { return preferred_output_id_; }
  void setPreferredInputId(const QString& id);
  void setPreferredOutputId(const QString& id);
  static QVariantList listInputDevices();
  static QVariantList listOutputDevices();

 signals:
  void devicesChanged();

 public slots:
  void onRemoteOpus(const QByteArray& packet);

 private slots:
  void onCaptureReady();

 private:
  bool openDevices();
  bool restartIfRunning();
  void pushCapturePcm(const int16_t* samples, int count);

  SendFn send_fn_;
  nyx::OpusEncoderWrap encoder_;
  nyx::OpusDecoderWrap decoder_;
  // Not Qt-parented: unique_ptr owns lifetime (avoids double-delete crash).
  std::unique_ptr<QAudioSource> source_;
  std::unique_ptr<QAudioSink> sink_;
  QIODevice* source_dev_ = nullptr;
  QIODevice* sink_dev_ = nullptr;
  QTimer* timer_ = nullptr;
  bool running_ = false;
  int capture_rate_ = nyx::kCallAudioSampleRate;
  int playback_rate_ = nyx::kCallAudioSampleRate;
  std::vector<int16_t> capture_pcm_;   // device-rate capture queue
  std::vector<int16_t> opus_pcm_;      // 48 kHz mono for Opus
  QString preferred_input_id_;
  QString preferred_output_id_;
};
