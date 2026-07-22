#include "call_audio_io.hpp"

#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#include <QTimer>

#if defined(NYX_HAS_QT_MULTIMEDIA)

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {
  timer_ = new QTimer(this);
  timer_->setInterval(nyx::kCallAudioFrameMs);
  connect(timer_, &QTimer::timeout, this, &CallAudioIo::onCaptureReady);
}

CallAudioIo::~CallAudioIo() { stop(); }

bool CallAudioIo::start() {
  if (running_) return true;
  if (!encoder_.ok() || !decoder_.ok()) return false;

  QAudioFormat fmt;
  fmt.setSampleRate(nyx::kCallAudioSampleRate);
  fmt.setChannelCount(nyx::kCallAudioChannels);
  fmt.setSampleFormat(QAudioFormat::Int16);

  const QAudioDevice in_dev = QMediaDevices::defaultAudioInput();
  const QAudioDevice out_dev = QMediaDevices::defaultAudioOutput();
  if (in_dev.isNull() || out_dev.isNull()) return false;

  source_ = std::make_unique<QAudioSource>(in_dev, fmt, this);
  sink_ = std::make_unique<QAudioSink>(out_dev, fmt, this);
  source_dev_ = source_->start();
  sink_dev_ = sink_->start();
  if (!source_dev_ || !sink_dev_) {
    stop();
    return false;
  }
  capture_buf_.clear();
  running_ = true;
  timer_->start();
  return true;
}

void CallAudioIo::stop() {
  running_ = false;
  if (timer_) timer_->stop();
  if (source_) {
    source_->stop();
    source_.reset();
  }
  if (sink_) {
    sink_->stop();
    sink_.reset();
  }
  source_dev_ = nullptr;
  sink_dev_ = nullptr;
  capture_buf_.clear();
}

void CallAudioIo::onCaptureReady() {
  if (!running_ || !source_dev_ || !send_fn_) return;
  const qint64 want_bytes =
      static_cast<qint64>(nyx::kCallAudioFrameSamples * sizeof(int16_t));
  while (source_dev_->bytesAvailable() >= want_bytes) {
    QByteArray chunk = source_dev_->read(want_bytes);
    if (chunk.size() < want_bytes) break;
    const auto* pcm = reinterpret_cast<const int16_t*>(chunk.constData());
    auto packet = encoder_.encode(pcm, nyx::kCallAudioFrameSamples);
    if (!packet) continue;
    send_fn_(*packet);
  }
}

void CallAudioIo::onRemoteOpus(const QByteArray& packet) {
  if (!running_ || !sink_dev_ || packet.isEmpty()) return;
  auto pcm = decoder_.decode(reinterpret_cast<const uint8_t*>(packet.constData()),
                             static_cast<std::size_t>(packet.size()));
  if (!pcm) return;
  sink_dev_->write(reinterpret_cast<const char*>(pcm->data()),
                   static_cast<qint64>(pcm->size() * sizeof(int16_t)));
}

#else

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {}
CallAudioIo::~CallAudioIo() = default;
bool CallAudioIo::start() { return false; }
void CallAudioIo::stop() {}
void CallAudioIo::onCaptureReady() {}
void CallAudioIo::onRemoteOpus(const QByteArray&) {}

#endif
