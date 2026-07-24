#pragma once

/** Захват/воспроизведение аудио звонка через Qt Multimedia + Opus. */

#include "nyx/call_opus.hpp"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <deque>
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

  void setSendFn(SendFn fn);
  /** Thread-safe: marshals onto this object's thread (must not block GUI). */
  Q_INVOKABLE bool start();
  Q_INVOKABLE void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  void setMuted(bool muted);
  bool muted() const { return muted_.load(std::memory_order_acquire); }

  /** True if recent send_fn_ returned false (throttled). */
  bool takeSendFailure() {
    return send_failed_.exchange(false);
  }

  QString preferredInputId() const { return preferred_input_id_; }
  QString preferredOutputId() const { return preferred_output_id_; }
  void setPreferredInputId(const QString& id);
  void setPreferredOutputId(const QString& id);
  static QVariantList listInputDevices();
  static QVariantList listOutputDevices();

  /** Settings: open mic only and report RMS level (0..1). Not for use during a call. */
  Q_INVOKABLE bool startMicLevelTest();
  Q_INVOKABLE void stopMicLevelTest();
  Q_INVOKABLE void playSpeakerTestTone();
  float micLevel() const { return mic_level_.load(std::memory_order_acquire); }
  bool micTestActive() const { return mic_test_.load(std::memory_order_acquire); }

 signals:
  void devicesChanged();
  void startFailed();
  void micLevelChanged();
  void micTestChanged();

 public slots:
  void onRemoteOpus(const QByteArray& packet);

 private slots:
  void onCaptureReady();

 private:
  bool openDevices();
  bool restartIfRunning();
  void pushCapturePcm(const int16_t* samples, int count);
  bool ensureOnAudioThread(const char* where);

  SendFn send_fn_;
  nyx::OpusEncoderWrap encoder_;
  nyx::OpusDecoderWrap decoder_;
  // Not Qt-parented: unique_ptr owns lifetime (avoids double-delete crash).
  std::unique_ptr<QAudioSource> source_;
  std::unique_ptr<QAudioSink> sink_;
  QIODevice* source_dev_ = nullptr;
  QIODevice* sink_dev_ = nullptr;
  QTimer* timer_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> muted_{false};
  std::atomic<bool> send_failed_{false};
  int capture_rate_ = nyx::kCallAudioSampleRate;
  int playback_rate_ = nyx::kCallAudioSampleRate;
  std::vector<int16_t> capture_pcm_;   // device-rate capture queue
  std::vector<int16_t> opus_pcm_;      // 48 kHz mono for Opus
  QString preferred_input_id_;
  QString preferred_output_id_;
  bool use_android_voice_track_ = false;
  bool use_android_voice_capture_ = false;
  std::deque<QByteArray> pending_remote_;
  std::atomic<bool> mic_test_{false};
  std::atomic<float> mic_level_{0.f};
  std::vector<int16_t> android_cap_scratch_;
};
