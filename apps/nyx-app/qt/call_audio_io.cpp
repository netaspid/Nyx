#include "call_audio_io.hpp"
#include "android_platform.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_ANDROID)
#include <android/log.h>
#define NYX_AUDIO_LOG(...) \
  __android_log_print(ANDROID_LOG_INFO, "NyxAudio", __VA_ARGS__)
#else
#define NYX_AUDIO_LOG(...) \
  do {                     \
  } while (0)
#endif

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
  QAudioFormat mono = near;
  mono.setChannelCount(1);
  mono.setSampleFormat(QAudioFormat::Int16);
  if (dev.isFormatSupported(mono)) return mono;
  return near;
}

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

bool CallAudioIo::ensureOnAudioThread(const char* where) {
  if (QThread::currentThread() == thread()) return true;
  NYX_AUDIO_LOG("marshal %s off-thread → audio thread", where ? where : "?");
  Q_UNUSED(where);
  return false;
}

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {
  timer_ = new QTimer(this);
  timer_->setInterval(nyx::kCallAudioFrameMs);
  connect(timer_, &QTimer::timeout, this, &CallAudioIo::onCaptureReady);
}

CallAudioIo::~CallAudioIo() { stop(); }

void CallAudioIo::setSendFn(SendFn fn) {
  if (!ensureOnAudioThread("setSendFn")) {
    QMetaObject::invokeMethod(
        this, [this, fn = std::move(fn)]() mutable { setSendFn(std::move(fn)); },
        Qt::QueuedConnection);
    return;
  }
  send_fn_ = std::move(fn);
}

void CallAudioIo::setMuted(bool muted) {
  muted_.store(muted, std::memory_order_release);
}

QVariantList CallAudioIo::listInputDevices() {
  return devicesToVariant(QMediaDevices::audioInputs());
}

QVariantList CallAudioIo::listOutputDevices() {
  return devicesToVariant(QMediaDevices::audioOutputs());
}

bool CallAudioIo::restartIfRunning() {
  if (!running_.load(std::memory_order_acquire)) return true;
  SendFn keep = send_fn_;
  stop();
  send_fn_ = std::move(keep);
  return start();
}

void CallAudioIo::setPreferredInputId(const QString& id) {
  if (!ensureOnAudioThread("setPreferredInputId")) {
    QMetaObject::invokeMethod(this, [this, id]() { setPreferredInputId(id); },
                              Qt::QueuedConnection);
    return;
  }
  preferred_input_id_ = id.trimmed();
  restartIfRunning();
  emit devicesChanged();
}

void CallAudioIo::setPreferredOutputId(const QString& id) {
  if (!ensureOnAudioThread("setPreferredOutputId")) {
    QMetaObject::invokeMethod(this, [this, id]() { setPreferredOutputId(id); },
                              Qt::QueuedConnection);
    return;
  }
  preferred_output_id_ = id.trimmed();
  restartIfRunning();
  emit devicesChanged();
}

bool CallAudioIo::openDevices() {
  NYX_AUDIO_LOG("openDevices begin");
  const QAudioDevice in_dev = findInput(preferred_input_id_);
  if (in_dev.isNull()) {
    NYX_AUDIO_LOG("openDevices: null input device");
    return false;
  }

  const QAudioFormat in_fmt = makeVoipFormat(in_dev);
  if (in_fmt.sampleFormat() != QAudioFormat::Int16) {
    NYX_AUDIO_LOG("openDevices: unsupported sample format");
    return false;
  }

  preferred_input_id_ = QString::fromUtf8(in_dev.id());
  capture_rate_ = in_fmt.sampleRate();

  source_ = std::make_unique<QAudioSource>(in_dev, in_fmt);
  const int bytes_20ms_in =
      in_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
  source_->setBufferSize(std::max(bytes_20ms_in * 10, 8192));

  NYX_AUDIO_LOG("openDevices: calling source start (may block this thread)");
  source_dev_ = source_->start();
  if (!source_dev_) {
    NYX_AUDIO_LOG("openDevices: source start failed");
    source_.reset();
    source_dev_ = nullptr;
    return false;
  }

#if defined(Q_OS_ANDROID)
  // QAudioSink uses STREAM_MUSIC — silent under MODE_IN_COMMUNICATION.
  use_android_voice_track_ = true;
  playback_rate_ = nyx::kCallAudioSampleRate;
  nyx_android::voice_playback_start(nyx::kCallAudioSampleRate, 1);
  preferred_output_id_.clear();
  NYX_AUDIO_LOG("openDevices ok (android AudioTrack voice) rate_in=%d", capture_rate_);
  return true;
#else
  use_android_voice_track_ = false;
  const QAudioDevice out_dev = findOutput(preferred_output_id_);
  if (out_dev.isNull()) {
    NYX_AUDIO_LOG("openDevices: null output device");
    source_->stop();
    source_.reset();
    source_dev_ = nullptr;
    return false;
  }
  const QAudioFormat out_fmt = makeVoipFormat(out_dev);
  if (out_fmt.sampleFormat() != QAudioFormat::Int16) {
    source_->stop();
    source_.reset();
    source_dev_ = nullptr;
    return false;
  }
  preferred_output_id_ = QString::fromUtf8(out_dev.id());
  playback_rate_ = out_fmt.sampleRate();
  sink_ = std::make_unique<QAudioSink>(out_dev, out_fmt);
  const int bytes_20ms_out =
      out_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
  sink_->setBufferSize(std::max(bytes_20ms_out * 10, 8192));
  sink_->setVolume(1.0);
  sink_dev_ = sink_->start();
  if (!sink_dev_) {
    NYX_AUDIO_LOG("openDevices: sink start failed");
    source_->stop();
    source_.reset();
    source_dev_ = nullptr;
    sink_.reset();
    return false;
  }
  NYX_AUDIO_LOG("openDevices ok rate_in=%d rate_out=%d", capture_rate_, playback_rate_);
  return true;
#endif
}

bool CallAudioIo::start() {
  if (!ensureOnAudioThread("start")) {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          if (!start()) emit startFailed();
        },
        Qt::QueuedConnection);
    return true;  // async — caller must not assume devices are open yet
  }
  if (running_.load(std::memory_order_acquire)) return true;
  if (!encoder_.ok() || !decoder_.ok()) {
    NYX_AUDIO_LOG("start: opus not ok");
    return false;
  }
  if (!openDevices()) {
    preferred_input_id_.clear();
    preferred_output_id_.clear();
    if (!openDevices()) {
      stop();
      return false;
    }
  }
  capture_pcm_.clear();
  opus_pcm_.clear();
  running_.store(true, std::memory_order_release);
  timer_->start();
  NYX_AUDIO_LOG("start ok");
  return true;
}

void CallAudioIo::stop() {
  if (!ensureOnAudioThread("stop")) {
    QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
    return;
  }
  NYX_AUDIO_LOG("stop");
  running_.store(false, std::memory_order_release);
  if (timer_) timer_->stop();
  if (source_) {
    source_->stop();
    source_.reset();
  }
  if (sink_) {
    sink_->stop();
    sink_.reset();
  }
  if (use_android_voice_track_) {
    nyx_android::voice_playback_stop();
    use_android_voice_track_ = false;
  }
  source_dev_ = nullptr;
  sink_dev_ = nullptr;
  capture_pcm_.clear();
  opus_pcm_.clear();
}

void CallAudioIo::pushCapturePcm(const int16_t* samples, int count) {
  if (!samples || count <= 0) return;
  capture_pcm_.insert(capture_pcm_.end(), samples, samples + count);

  const int need_dev =
      std::max(1, (nyx::kCallAudioFrameSamples * capture_rate_ + nyx::kCallAudioSampleRate - 1) /
                      nyx::kCallAudioSampleRate);
  while (static_cast<int>(capture_pcm_.size()) >= need_dev) {
    std::vector<int16_t> frame48;
    resampleMono(capture_pcm_.data(), need_dev, capture_rate_, &frame48,
                 nyx::kCallAudioSampleRate);
    capture_pcm_.erase(capture_pcm_.begin(), capture_pcm_.begin() + need_dev);
    if (static_cast<int>(frame48.size()) < nyx::kCallAudioFrameSamples) {
      frame48.resize(static_cast<std::size_t>(nyx::kCallAudioFrameSamples), 0);
    }
    opus_pcm_.insert(opus_pcm_.end(), frame48.begin(),
                     frame48.begin() + nyx::kCallAudioFrameSamples);
  }

  while (static_cast<int>(opus_pcm_.size()) >= nyx::kCallAudioFrameSamples) {
    if (muted_.load(std::memory_order_acquire)) {
      std::fill(opus_pcm_.begin(), opus_pcm_.begin() + nyx::kCallAudioFrameSamples, 0);
    }
    auto packet = encoder_.encode(opus_pcm_.data(), nyx::kCallAudioFrameSamples);
    opus_pcm_.erase(opus_pcm_.begin(),
                    opus_pcm_.begin() + nyx::kCallAudioFrameSamples);
    if (!packet || !send_fn_) continue;
    if (!send_fn_(*packet)) send_failed_.store(true, std::memory_order_release);
  }
}

void CallAudioIo::onCaptureReady() {
  if (!running_.load(std::memory_order_acquire) || !source_dev_ || !send_fn_) return;
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
      const int32_t a = interleaved[i * channels];
      const int32_t b = interleaved[i * channels + 1];
      mono[static_cast<std::size_t>(i)] = static_cast<int16_t>((a + b) / 2);
    }
  }
  pushCapturePcm(mono.data(), frames);
}

void CallAudioIo::onRemoteOpus(const QByteArray& packet) {
  if (!ensureOnAudioThread("onRemoteOpus")) {
    QMetaObject::invokeMethod(this, [this, packet]() { onRemoteOpus(packet); },
                              Qt::QueuedConnection);
    return;
  }
  if (!running_.load(std::memory_order_acquire) || packet.isEmpty()) return;
  auto pcm = decoder_.decode(reinterpret_cast<const uint8_t*>(packet.constData()),
                             static_cast<std::size_t>(packet.size()));
  if (!pcm || pcm->empty()) return;

  if (use_android_voice_track_) {
    // Always 48 kHz mono into AudioTrack VOICE_COMMUNICATION.
    nyx_android::voice_playback_write(pcm->data(), static_cast<int>(pcm->size()));
    return;
  }

  if (!sink_dev_) return;

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
void CallAudioIo::setSendFn(SendFn) {}
bool CallAudioIo::start() { return false; }
void CallAudioIo::stop() {}
void CallAudioIo::setMuted(bool) {}
void CallAudioIo::onCaptureReady() {}
void CallAudioIo::onRemoteOpus(const QByteArray&) {}
void CallAudioIo::setPreferredInputId(const QString&) {}
void CallAudioIo::setPreferredOutputId(const QString&) {}
QVariantList CallAudioIo::listInputDevices() { return {}; }
QVariantList CallAudioIo::listOutputDevices() { return {}; }
bool CallAudioIo::openDevices() { return false; }
bool CallAudioIo::restartIfRunning() { return false; }
void CallAudioIo::pushCapturePcm(const int16_t*, int) {}
bool CallAudioIo::ensureOnAudioThread(const char*) { return true; }

#endif
