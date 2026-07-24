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
#include <climits>
#include <cmath>
#include <memory>

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

bool looksLikeMonitor(const QAudioDevice& d) {
  const QString t = d.description().toLower();
  return t.contains(QStringLiteral("monitor")) || t.contains(QStringLiteral("loopback")) ||
         t.contains(QStringLiteral("what u hear"));
}

QAudioDevice findInput(const QString& id) {
  const auto list = QMediaDevices::audioInputs();
  if (!id.isEmpty()) {
    for (const auto& d : list) {
      if (QString::fromUtf8(d.id()) == id) return d;
    }
  }
  // Prefer OS default (USB headset mic etc.), skip pulse "Monitor of …".
  const QAudioDevice def = QMediaDevices::defaultAudioInput();
  if (!def.isNull() && !looksLikeMonitor(def)) return def;
  for (const auto& d : list) {
    if (!looksLikeMonitor(d)) return d;
  }
  return def;
}

QList<QAudioDevice> inputCandidates(const QString& preferred_id) {
  QList<QAudioDevice> out;
  auto add = [&](const QAudioDevice& d) {
    if (d.isNull()) return;
    for (const auto& x : out) {
      if (x.id() == d.id()) return;
    }
    out.append(d);
  };
  if (!preferred_id.isEmpty()) add(findInput(preferred_id));
  add(QMediaDevices::defaultAudioInput());
  for (const auto& d : QMediaDevices::audioInputs()) {
    if (!looksLikeMonitor(d)) add(d);
  }
  return out;
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
  if (dev.isNull()) {
    QAudioFormat want;
    want.setSampleRate(nyx::kCallAudioSampleRate);
    want.setChannelCount(nyx::kCallAudioChannels);
    want.setSampleFormat(QAudioFormat::Int16);
    return want;
  }
  // Try common VoIP rates first — USB headsets often reject 48 kHz mono.
  const int rates[] = {nyx::kCallAudioSampleRate, 44100, 32000, 16000, 8000};
  for (int rate : rates) {
    QAudioFormat want;
    want.setSampleRate(rate);
    want.setChannelCount(1);
    want.setSampleFormat(QAudioFormat::Int16);
    if (dev.isFormatSupported(want)) return want;
    QAudioFormat stereo = want;
    stereo.setChannelCount(2);
    if (dev.isFormatSupported(stereo)) return stereo;
  }
  QAudioFormat near = dev.preferredFormat();
  if (near.channelCount() <= 0) near.setChannelCount(1);
  if (near.sampleRate() <= 0) near.setSampleRate(nyx::kCallAudioSampleRate);
  return near;
}

std::vector<int16_t> audioBytesToMono16(const QByteArray& bytes, const QAudioFormat& fmt) {
  const int channels = std::max(1, fmt.channelCount());
  const int bytes_per_sample = fmt.bytesPerSample();
  if (bytes_per_sample <= 0) return {};
  const int frames = bytes.size() / (bytes_per_sample * channels);
  if (frames <= 0) return {};
  std::vector<int16_t> mono(static_cast<std::size_t>(frames));

  for (int i = 0; i < frames; ++i) {
    int64_t sum = 0;
    for (int c = 0; c < channels; ++c) {
      const int index = i * channels + c;
      int32_t sample = 0;
      switch (fmt.sampleFormat()) {
        case QAudioFormat::Int16:
          sample = reinterpret_cast<const int16_t*>(bytes.constData())[index];
          break;
        case QAudioFormat::Int32:
          sample = reinterpret_cast<const int32_t*>(bytes.constData())[index] >> 16;
          break;
        case QAudioFormat::Float: {
          const float value = reinterpret_cast<const float*>(bytes.constData())[index];
          sample = static_cast<int32_t>(
              std::clamp(value, -1.0f, 1.0f) * static_cast<float>(INT16_MAX));
          break;
        }
        case QAudioFormat::UInt8:
          sample = (static_cast<int32_t>(
                        reinterpret_cast<const uint8_t*>(bytes.constData())[index]) -
                    128)
                   << 8;
          break;
        default:
          return {};
      }
      sum += sample;
    }
    mono[static_cast<std::size_t>(i)] =
        static_cast<int16_t>(std::clamp<int64_t>(sum / channels, INT16_MIN, INT16_MAX));
  }
  return mono;
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

#if defined(Q_OS_ANDROID)
  // Native AudioRecord — Qt QAudioSource is often silent under MODE_IN_COMMUNICATION.
  use_android_voice_capture_ = false;
  capture_rate_ = nyx::kCallAudioSampleRate;
  if (!nyx_android::voice_capture_start(nyx::kCallAudioSampleRate, 1)) {
    NYX_AUDIO_LOG("openDevices: AudioRecord start failed");
    return false;
  }
  use_android_voice_capture_ = true;
  source_.reset();
  source_dev_ = nullptr;
  preferred_input_id_.clear();

  if (mic_test_.load(std::memory_order_acquire)) {
    use_android_voice_track_ = false;
    NYX_AUDIO_LOG("openDevices ok (android AudioRecord mic test)");
    return true;
  }

  use_android_voice_track_ = true;
  playback_rate_ = nyx::kCallAudioSampleRate;
  nyx_android::voice_playback_start(nyx::kCallAudioSampleRate, 1);
  preferred_output_id_.clear();
  nyx_android::set_speakerphone(nyx_android::speakerphone());
  NYX_AUDIO_LOG("openDevices ok (android AudioRecord+AudioTrack)");
  return true;
#else
  source_.reset();
  source_dev_ = nullptr;
  bool opened_in = false;
  for (const QAudioDevice& in_dev : inputCandidates(preferred_input_id_)) {
    if (in_dev.isNull()) continue;
    const QAudioFormat in_fmt = makeVoipFormat(in_dev);
    if (in_fmt.sampleFormat() == QAudioFormat::Unknown) continue;
    auto src = std::make_unique<QAudioSource>(in_dev, in_fmt);
    const int bytes_20ms_in =
        in_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
    src->setBufferSize(std::max(bytes_20ms_in * 10, 8192));
    QIODevice* dev = src->start();
    if (!dev) {
      NYX_AUDIO_LOG("openDevices: input failed id=%s", qPrintable(QString::fromUtf8(in_dev.id())));
      continue;
    }
    source_ = std::move(src);
    source_dev_ = dev;
    preferred_input_id_ = QString::fromUtf8(in_dev.id());
    capture_rate_ = in_fmt.sampleRate();
    opened_in = true;
    NYX_AUDIO_LOG("openDevices: input ok desc=%s rate=%d ch=%d",
                  qPrintable(in_dev.description()), capture_rate_, in_fmt.channelCount());
    break;
  }
  if (!opened_in) {
    NYX_AUDIO_LOG("openDevices: no usable input device");
    return false;
  }

  if (mic_test_.load(std::memory_order_acquire)) {
    use_android_voice_track_ = false;
    NYX_AUDIO_LOG("openDevices ok (mic test) rate_in=%d", capture_rate_);
    return true;
  }

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
  // Drain packets that arrived before the audio thread finished opening devices.
  while (!pending_remote_.empty()) {
    const QByteArray pkt = pending_remote_.front();
    pending_remote_.pop_front();
    onRemoteOpus(pkt);
  }
  NYX_AUDIO_LOG("start ok");
  return true;
}

void CallAudioIo::stop() {
  if (!ensureOnAudioThread("stop")) {
    QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
    return;
  }
  NYX_AUDIO_LOG("stop");
  const bool was_mic_test = mic_test_.exchange(false, std::memory_order_acq_rel);
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
  if (use_android_voice_capture_) {
    nyx_android::voice_capture_stop();
    use_android_voice_capture_ = false;
  }
  source_dev_ = nullptr;
  sink_dev_ = nullptr;
  capture_pcm_.clear();
  opus_pcm_.clear();
  pending_remote_.clear();
  android_cap_scratch_.clear();
  mic_level_.store(0.f, std::memory_order_release);
  if (was_mic_test) {
    emit micLevelChanged();
    emit micTestChanged();
  }
}

void CallAudioIo::pushCapturePcm(const int16_t* samples, int count) {
  if (!samples || count <= 0) return;

  if (mic_test_.load(std::memory_order_acquire)) {
    double sum = 0.0;
    for (int i = 0; i < count; ++i) {
      const double s = static_cast<double>(samples[i]) / 32768.0;
      sum += s * s;
    }
    const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(count)));
    const float level = std::clamp(rms * 4.5f, 0.f, 1.f);
    mic_level_.store(level, std::memory_order_release);
    emit micLevelChanged();
    return;
  }

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
  if (!running_.load(std::memory_order_acquire)) return;
  if (!send_fn_ && !mic_test_.load(std::memory_order_acquire)) return;

#if defined(Q_OS_ANDROID)
  if (use_android_voice_capture_) {
    // Drain AudioRecord aggressively so the mic buffer does not overrun.
    android_cap_scratch_.resize(static_cast<std::size_t>(nyx::kCallAudioFrameSamples * 4));
    for (int round = 0; round < 8; ++round) {
      const int n = nyx_android::voice_capture_read(android_cap_scratch_.data(),
                                                    static_cast<int>(android_cap_scratch_.size()));
      if (n <= 0) break;
      pushCapturePcm(android_cap_scratch_.data(), n);
    }
    return;
  }
#endif

  if (!source_dev_) return;
  const QAudioFormat format = source_ ? source_->format() : QAudioFormat{};
  const int channels = std::max(1, format.channelCount());
  const int bytes_per_sample = std::max(1, format.bytesPerSample());
  const qint64 avail = source_dev_->bytesAvailable();
  if (avail < static_cast<qint64>(bytes_per_sample * channels)) return;

  QByteArray chunk = source_dev_->read(avail);
  if (chunk.isEmpty()) return;
  auto mono = audioBytesToMono16(chunk, format);
  if (!mono.empty()) pushCapturePcm(mono.data(), static_cast<int>(mono.size()));
}

bool CallAudioIo::startMicLevelTest() {
  if (!ensureOnAudioThread("startMicLevelTest")) {
    QMetaObject::invokeMethod(
        this,
        [this]() {
          if (!startMicLevelTest()) emit startFailed();
        },
        Qt::QueuedConnection);
    return true;
  }
  if (running_.load(std::memory_order_acquire)) {
    NYX_AUDIO_LOG("startMicLevelTest: already running");
    return false;
  }
  mic_test_.store(true, std::memory_order_release);
  mic_level_.store(0.f, std::memory_order_release);
  send_fn_ = nullptr;
  if (!openDevices()) {
    preferred_input_id_.clear();
    if (!openDevices()) {
      mic_test_.store(false, std::memory_order_release);
      stop();
      return false;
    }
  }
  running_.store(true, std::memory_order_release);
  timer_->start();
  emit micTestChanged();
  emit micLevelChanged();
  NYX_AUDIO_LOG("mic level test started");
  return true;
}

void CallAudioIo::stopMicLevelTest() {
  if (!ensureOnAudioThread("stopMicLevelTest")) {
    QMetaObject::invokeMethod(this, [this]() { stopMicLevelTest(); }, Qt::QueuedConnection);
    return;
  }
  if (!mic_test_.load(std::memory_order_acquire) &&
      !running_.load(std::memory_order_acquire))
    return;
  stop();
}

void CallAudioIo::playSpeakerTestTone() {
#if defined(Q_OS_ANDROID)
  nyx_android::play_test_tone(nyx::kCallAudioSampleRate, 700);
#else
  if (!ensureOnAudioThread("playSpeakerTestTone")) {
    QMetaObject::invokeMethod(this, [this]() { playSpeakerTestTone(); }, Qt::QueuedConnection);
    return;
  }
  const QAudioDevice out_dev = findOutput(preferred_output_id_);
  if (out_dev.isNull()) return;
  QAudioFormat fmt = makeVoipFormat(out_dev);
  fmt.setSampleFormat(QAudioFormat::Int16);
  fmt.setChannelCount(1);
  auto sink = std::make_unique<QAudioSink>(out_dev, fmt);
  sink->setVolume(1.0);
  QIODevice* dev = sink->start();
  if (!dev) return;
  const int sr = fmt.sampleRate() > 0 ? fmt.sampleRate() : nyx::kCallAudioSampleRate;
  const int n = sr * 700 / 1000;
  std::vector<int16_t> pcm(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(sr);
    pcm[static_cast<std::size_t>(i)] =
        static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * 440.0 * t) * 12000.0);
  }
  dev->write(reinterpret_cast<const char*>(pcm.data()),
             static_cast<qint64>(pcm.size() * sizeof(int16_t)));
  QTimer::singleShot(800, this, [s = std::shared_ptr<QAudioSink>(std::move(sink))]() {
    if (s) s->stop();
  });
#endif
}

void CallAudioIo::onRemoteOpus(const QByteArray& packet) {
  if (!ensureOnAudioThread("onRemoteOpus")) {
    QMetaObject::invokeMethod(this, [this, packet]() { onRemoteOpus(packet); },
                              Qt::QueuedConnection);
    return;
  }
  if (packet.isEmpty()) return;
  if (!running_.load(std::memory_order_acquire)) {
    pending_remote_.push_back(packet);
    while (pending_remote_.size() > 80) pending_remote_.pop_front();
    return;
  }
  auto pcm = decoder_.decode(reinterpret_cast<const uint8_t*>(packet.constData()),
                             static_cast<std::size_t>(packet.size()));
  if (!pcm || pcm->empty()) {
    NYX_AUDIO_LOG("onRemoteOpus decode failed bytes=%d", int(packet.size()));
    return;
  }

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
bool CallAudioIo::startMicLevelTest() { return false; }
void CallAudioIo::stopMicLevelTest() {}
void CallAudioIo::playSpeakerTestTone() {}

#endif
