#include "call_audio_io.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

#if defined(NYX_HAS_QT_MULTIMEDIA)

namespace {

QAudioDevice findInput(const QString& id) {
  const auto list = QMediaDevices::audioInputs();
  if (!id.isEmpty()) {
    for (const auto& d : list) {
      if (QString::fromUtf8(d.id()) == id) return d;
    }
  }
  return QMediaDevices::defaultAudioInput();
}

QAudioDevice findOutput(const QString& id) {
  const auto list = QMediaDevices::audioOutputs();
  if (!id.isEmpty()) {
    for (const auto& d : list) {
      if (QString::fromUtf8(d.id()) == id) return d;
    }
  }
  return QMediaDevices::defaultAudioOutput();
}

QVariantList devicesToVariant(const QList<QAudioDevice>& list) {
  QVariantList out;
  for (const auto& d : list) {
    QVariantMap row;
    row.insert(QStringLiteral("id"), QString::fromUtf8(d.id()));
    QString text = d.description();
    if (text.isEmpty()) text = QString::fromUtf8(d.id());
    if (d.isDefault()) text += QStringLiteral(" (по умолчанию)");
    row.insert(QStringLiteral("text"), text);
    out.append(row);
  }
  return out;
}

QAudioFormat makeVoipFormat(const QAudioDevice& dev) {
  QAudioFormat want;
  want.setSampleRate(nyx::kCallAudioSampleRate);
  want.setChannelCount(nyx::kCallAudioChannels);
  want.setSampleFormat(QAudioFormat::Int16);
  if (dev.isNull()) return want;
  if (dev.isFormatSupported(want)) return want;
  QAudioFormat near = dev.preferredFormat();
  if (near.sampleFormat() == QAudioFormat::Unknown)
    near.setSampleFormat(QAudioFormat::Int16);
  else if (near.sampleFormat() != QAudioFormat::Int16)
    near.setSampleFormat(QAudioFormat::Int16);
  if (near.channelCount() <= 0) near.setChannelCount(1);
  if (near.sampleRate() <= 0) near.setSampleRate(nyx::kCallAudioSampleRate);
  // Prefer mono for VoIP if device allows.
  QAudioFormat mono = near;
  mono.setChannelCount(1);
  mono.setSampleFormat(QAudioFormat::Int16);
  if (dev.isFormatSupported(mono)) return mono;
  return near;
}

/** Linear resample mono int16 → mono int16. */
void resampleMono(const int16_t* in, int in_n, int in_rate, std::vector<int16_t>* out,
                  int out_rate) {
  out->clear();
  if (!in || in_n <= 0 || in_rate <= 0 || out_rate <= 0) return;
  if (in_rate == out_rate) {
    out->assign(in, in + in_n);
    return;
  }
  const double ratio = static_cast<double>(out_rate) / static_cast<double>(in_rate);
  const int out_n = std::max(1, static_cast<int>(std::lround(in_n * ratio)));
  out->resize(static_cast<std::size_t>(out_n));
  for (int i = 0; i < out_n; ++i) {
    const double src = static_cast<double>(i) / ratio;
    const int i0 = static_cast<int>(src);
    const int i1 = std::min(i0 + 1, in_n - 1);
    const double t = src - static_cast<double>(i0);
    const double s =
        (1.0 - t) * static_cast<double>(in[i0]) + t * static_cast<double>(in[i1]);
    (*out)[static_cast<std::size_t>(i)] =
        static_cast<int16_t>(std::clamp(s, -32768.0, 32767.0));
  }
}

}  // namespace

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {
  timer_ = new QTimer(this);
  timer_->setInterval(nyx::kCallAudioFrameMs);
  connect(timer_, &QTimer::timeout, this, &CallAudioIo::onCaptureReady);
}

CallAudioIo::~CallAudioIo() { stop(); }

QVariantList CallAudioIo::listInputDevices() {
  return devicesToVariant(QMediaDevices::audioInputs());
}

QVariantList CallAudioIo::listOutputDevices() {
  return devicesToVariant(QMediaDevices::audioOutputs());
}

bool CallAudioIo::restartIfRunning() {
  if (!running_) return true;
  SendFn keep = send_fn_;
  stop();
  send_fn_ = std::move(keep);
  return start();
}

void CallAudioIo::setPreferredInputId(const QString& id) {
  preferred_input_id_ = id.trimmed();
  restartIfRunning();
  emit devicesChanged();
}

void CallAudioIo::setPreferredOutputId(const QString& id) {
  preferred_output_id_ = id.trimmed();
  restartIfRunning();
  emit devicesChanged();
}

bool CallAudioIo::openDevices() {
  const QAudioDevice in_dev = findInput(preferred_input_id_);
  const QAudioDevice out_dev = findOutput(preferred_output_id_);
  if (in_dev.isNull() || out_dev.isNull()) return false;

  const QAudioFormat in_fmt = makeVoipFormat(in_dev);
  const QAudioFormat out_fmt = makeVoipFormat(out_dev);
  if (in_fmt.sampleFormat() != QAudioFormat::Int16 ||
      out_fmt.sampleFormat() != QAudioFormat::Int16) {
    return false;
  }

  preferred_input_id_ = QString::fromUtf8(in_dev.id());
  preferred_output_id_ = QString::fromUtf8(out_dev.id());
  capture_rate_ = in_fmt.sampleRate();
  playback_rate_ = out_fmt.sampleRate();

  // No QObject parent — unique_ptr is sole owner (prevents double-free crash on stop).
  source_ = std::make_unique<QAudioSource>(in_dev, in_fmt);
  sink_ = std::make_unique<QAudioSink>(out_dev, out_fmt);

  const int bytes_20ms_in =
      in_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
  const int bytes_20ms_out =
      out_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
  source_->setBufferSize(std::max(bytes_20ms_in * 10, 8192));
  sink_->setBufferSize(std::max(bytes_20ms_out * 10, 8192));

  source_dev_ = source_->start();
  sink_dev_ = sink_->start();
  if (!source_dev_ || !sink_dev_) {
    source_.reset();
    sink_.reset();
    source_dev_ = nullptr;
    sink_dev_ = nullptr;
    return false;
  }
  return true;
}

bool CallAudioIo::start() {
  if (running_) return true;
  if (!encoder_.ok() || !decoder_.ok()) return false;
  if (!openDevices()) {
    // Stale saved device ids (common after replug / Android) — fall back to defaults.
    preferred_input_id_.clear();
    preferred_output_id_.clear();
    if (!openDevices()) {
      stop();
      return false;
    }
  }
  capture_pcm_.clear();
  opus_pcm_.clear();
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
  capture_pcm_.clear();
  opus_pcm_.clear();
}

void CallAudioIo::pushCapturePcm(const int16_t* samples, int count) {
  if (!samples || count <= 0) return;
  capture_pcm_.insert(capture_pcm_.end(), samples, samples + count);

  // Resample whole queued buffer when we have enough for ≥1 Opus frame at 48 kHz.
  const int need_dev =
      std::max(1, (nyx::kCallAudioFrameSamples * capture_rate_ + nyx::kCallAudioSampleRate - 1) /
                      nyx::kCallAudioSampleRate);
  while (static_cast<int>(capture_pcm_.size()) >= need_dev) {
    std::vector<int16_t> frame48;
    resampleMono(capture_pcm_.data(), need_dev, capture_rate_, &frame48,
                 nyx::kCallAudioSampleRate);
    // Drop used device samples.
    capture_pcm_.erase(capture_pcm_.begin(),
                       capture_pcm_.begin() + need_dev);
    if (static_cast<int>(frame48.size()) < nyx::kCallAudioFrameSamples) {
      frame48.resize(static_cast<std::size_t>(nyx::kCallAudioFrameSamples), 0);
    }
    opus_pcm_.insert(opus_pcm_.end(), frame48.begin(),
                     frame48.begin() + nyx::kCallAudioFrameSamples);
  }

  while (static_cast<int>(opus_pcm_.size()) >= nyx::kCallAudioFrameSamples) {
    auto packet = encoder_.encode(opus_pcm_.data(), nyx::kCallAudioFrameSamples);
    opus_pcm_.erase(opus_pcm_.begin(),
                    opus_pcm_.begin() + nyx::kCallAudioFrameSamples);
    if (!packet || !send_fn_) continue;
    send_fn_(*packet);
  }
}

void CallAudioIo::onCaptureReady() {
  if (!running_ || !source_dev_ || !send_fn_) return;
  const int ch = source_ ? source_->format().channelCount() : 1;
  const int channels = std::max(1, ch);
  const qint64 avail = source_dev_->bytesAvailable();
  if (avail < static_cast<qint64>(sizeof(int16_t) * channels)) return;

  QByteArray chunk = source_dev_->read(avail);
  if (chunk.isEmpty()) return;
  const int frames = chunk.size() / static_cast<int>(sizeof(int16_t) * channels);
  if (frames <= 0) return;

  const auto* interleaved = reinterpret_cast<const int16_t*>(chunk.constData());
  std::vector<int16_t> mono(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    if (channels == 1) {
      mono[static_cast<std::size_t>(i)] = interleaved[i];
    } else {
      // Downmix first two channels.
      const int32_t a = interleaved[i * channels];
      const int32_t b = interleaved[i * channels + 1];
      mono[static_cast<std::size_t>(i)] = static_cast<int16_t>((a + b) / 2);
    }
  }
  pushCapturePcm(mono.data(), frames);
}

void CallAudioIo::onRemoteOpus(const QByteArray& packet) {
  if (!running_ || !sink_dev_ || packet.isEmpty()) return;
  auto pcm = decoder_.decode(reinterpret_cast<const uint8_t*>(packet.constData()),
                             static_cast<std::size_t>(packet.size()));
  if (!pcm || pcm->empty()) return;

  if (playback_rate_ == nyx::kCallAudioSampleRate &&
      sink_->format().channelCount() == 1) {
    sink_dev_->write(reinterpret_cast<const char*>(pcm->data()),
                     static_cast<qint64>(pcm->size() * sizeof(int16_t)));
    return;
  }

  std::vector<int16_t> play;
  resampleMono(pcm->data(), static_cast<int>(pcm->size()), nyx::kCallAudioSampleRate, &play,
               playback_rate_);
  const int ch = std::max(1, sink_->format().channelCount());
  if (ch == 1) {
    sink_dev_->write(reinterpret_cast<const char*>(play.data()),
                     static_cast<qint64>(play.size() * sizeof(int16_t)));
    return;
  }
  std::vector<int16_t> interleaved(play.size() * static_cast<std::size_t>(ch));
  for (std::size_t i = 0; i < play.size(); ++i) {
    for (int c = 0; c < ch; ++c)
      interleaved[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = play[i];
  }
  sink_dev_->write(reinterpret_cast<const char*>(interleaved.data()),
                   static_cast<qint64>(interleaved.size() * sizeof(int16_t)));
}

#else

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {}
CallAudioIo::~CallAudioIo() = default;
bool CallAudioIo::start() { return false; }
void CallAudioIo::stop() {}
void CallAudioIo::onCaptureReady() {}
void CallAudioIo::onRemoteOpus(const QByteArray&) {}
void CallAudioIo::setPreferredInputId(const QString&) {}
void CallAudioIo::setPreferredOutputId(const QString&) {}
QVariantList CallAudioIo::listInputDevices() { return {}; }
QVariantList CallAudioIo::listOutputDevices() { return {}; }
bool CallAudioIo::openDevices() { return false; }
bool CallAudioIo::restartIfRunning() { return false; }
void CallAudioIo::pushCapturePcm(const int16_t*, int) {}

#endif
