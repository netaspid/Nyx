#pragma once

#include "nyx/call_opus.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
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

  Q_INVOKABLE bool start();
  Q_INVOKABLE void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }

  void setMuted(bool muted);
  bool muted() const { return muted_.load(std::memory_order_acquire); }

  bool takeSendFailure() { return send_failed_.exchange(false); }

  QString preferredInputId() const { return preferred_input_id_; }
  QString preferredOutputId() const { return preferred_output_id_; }
  void setPreferredInputId(const QString& id);
  void setPreferredOutputId(const QString& id);
  static QVariantList listInputDevices();
  static QVariantList listOutputDevices();

  Q_INVOKABLE bool startMicLevelTest();
  Q_INVOKABLE void stopMicLevelTest();
  Q_INVOKABLE void playSpeakerTestTone();
  float micLevel() const { return mic_level_.load(std::memory_order_acquire); }
  bool micTestActive() const { return mic_test_.load(std::memory_order_acquire); }
  bool localVoiceActive() const { return local_voice_active_.load(std::memory_order_acquire); }
  uint8_t localVoiceLevel() const { return local_voice_level_.load(std::memory_order_acquire); }

signals:
  void devicesChanged();
  void startFailed();
  void micLevelChanged();
  void micTestChanged();
  void localVoiceActiveChanged(bool active);
  void dominantSpeakerChanged(const QString& peerId);

public slots:
  void onRemoteOpus(const QString& peerId, const QByteArray& packet);

private slots:
  void onCaptureReady();

private:
  bool openDevices();
  bool restartIfRunning();
  void pushCapturePcm(const int16_t* samples, int count);
  void mixRemoteAudio();
  void writePlayback(const std::vector<int16_t>& pcm);
  bool ensureOnAudioThread(const char* where);

  SendFn send_fn_;
  nyx::OpusEncoderWrap encoder_;
  struct PeerAudio {
    std::unique_ptr<nyx::OpusDecoderWrap> decoder;
    std::deque<std::vector<int16_t>> jitter;
    float level = 0.f;
    qint64 last_packet_ms = 0;
    bool primed = false;
  };
  std::map<QString, PeerAudio> remote_peers_;

  std::unique_ptr<QAudioSource> source_;
  std::unique_ptr<QAudioSink> sink_;
  QIODevice* source_dev_ = nullptr;
  QIODevice* sink_dev_ = nullptr;
  void* pulse_capture_ = nullptr;
  QTimer* timer_ = nullptr;
  std::atomic<bool> running_ {false};
  std::atomic<bool> muted_ {false};
  std::atomic<bool> send_failed_ {false};
  int capture_rate_ = nyx::kCallAudioSampleRate;
  int playback_rate_ = nyx::kCallAudioSampleRate;
  std::vector<int16_t> capture_pcm_;
  std::vector<int16_t> opus_pcm_;
  QString preferred_input_id_;
  QString preferred_output_id_;
  bool use_android_voice_track_ = false;
  bool use_android_voice_capture_ = false;
  std::deque<std::pair<QString, QByteArray>> pending_remote_;
  std::atomic<bool> mic_test_ {false};
  std::atomic<float> mic_level_ {0.f};
  std::atomic<bool> local_voice_active_ {false};
  std::atomic<uint8_t> local_voice_level_ {0};
  qint64 local_voice_last_ms_ = 0;
  QString dominant_speaker_;
  qint64 dominant_since_ms_ = 0;
  qint64 dominant_last_voice_ms_ = 0;
  std::vector<int16_t> android_cap_scratch_;
};
