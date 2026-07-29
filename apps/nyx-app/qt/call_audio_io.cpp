#include "call_audio_io.hpp"
#include "android_platform.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QDateTime>
#include <QDebug>
#include <QMediaDevices>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <memory>

#if defined(NYX_HAS_PULSE_SIMPLE)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#if defined(Q_OS_ANDROID)
#include <android/log.h>
#define NYX_AUDIO_LOG(...) \
  __android_log_print(ANDROID_LOG_INFO, "NyxAudio", __VA_ARGS__)
#else
#define NYX_AUDIO_LOG(...) \
  qInfo().noquote() << QStringLiteral("NyxAudio:") << QString::asprintf(__VA_ARGS__)
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

QAudioFormat makeCaptureFormat(const QAudioDevice& dev) {
  if (dev.isNull()) return makeVoipFormat(dev);
  QAudioFormat native = dev.preferredFormat();
  if (native.sampleRate() > 0 && native.channelCount() > 0 &&
      native.sampleFormat() != QAudioFormat::Unknown &&
      dev.isFormatSupported(native)) {
    return native;
  }
  return makeVoipFormat(dev);
}

std::vector<int16_t> audioBytesToMono16(const QByteArray& bytes, const QAudioFormat& fmt) {
  const int channels = std::max(1, fmt.channelCount());
  const int bytes_per_sample = fmt.bytesPerSample();
  if (bytes_per_sample <= 0) return {};
  const int frames = bytes.size() / (bytes_per_sample * channels);
  if (frames <= 0) return {};
  auto sample_at = [&](int index) -> int32_t {
    switch (fmt.sampleFormat()) {
      case QAudioFormat::Int16:
        return reinterpret_cast<const int16_t*>(bytes.constData())[index];
      case QAudioFormat::Int32:
        return reinterpret_cast<const int32_t*>(bytes.constData())[index] >> 16;
      case QAudioFormat::Float: {
        const float value = reinterpret_cast<const float*>(bytes.constData())[index];
        if (!std::isfinite(value)) return 0;
        return static_cast<int32_t>(
            std::clamp(value, -1.0f, 1.0f) * static_cast<float>(INT16_MAX));
      }
      case QAudioFormat::UInt8:
        return (static_cast<int32_t>(
                    reinterpret_cast<const uint8_t*>(bytes.constData())[index]) -
                128)
               << 8;
      default:
        return 0;
    }
  };
  if (fmt.sampleFormat() == QAudioFormat::Unknown) return {};

  int selected_channel = 0;
  if (channels > 1) {
    std::vector<double> energy(static_cast<std::size_t>(channels), 0.0);
    const int inspect_frames = std::min(frames, 4096);
    for (int i = 0; i < inspect_frames; ++i) {
      for (int c = 0; c < channels; ++c) {
        const double sample = sample_at(i * channels + c);
        energy[static_cast<std::size_t>(c)] += sample * sample;
      }
    }
    selected_channel = static_cast<int>(
        std::max_element(energy.begin(), energy.end()) - energy.begin());
  }

  std::vector<int16_t> mono(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    mono[static_cast<std::size_t>(i)] = static_cast<int16_t>(
        std::clamp<int32_t>(sample_at(i * channels + selected_channel),
                            INT16_MIN, INT16_MAX));
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
  const auto close_input = [this]() {
    if (source_) {
      source_->stop();
      source_.reset();
    }
    source_dev_ = nullptr;
#if defined(NYX_HAS_PULSE_SIMPLE)
    if (pulse_capture_) {
      pa_simple_free(static_cast<pa_simple*>(pulse_capture_));
      pulse_capture_ = nullptr;
    }
#endif
  };
  close_input();
  bool opened_in = false;

#if defined(NYX_HAS_PULSE_SIMPLE)
  const pa_sample_spec pulse_spec{
      PA_SAMPLE_S16LE, static_cast<uint32_t>(nyx::kCallAudioSampleRate), 1};
  pa_buffer_attr pulse_attr{};
  pulse_attr.maxlength = static_cast<uint32_t>(-1);
  pulse_attr.tlength = static_cast<uint32_t>(-1);
  pulse_attr.prebuf = static_cast<uint32_t>(-1);
  pulse_attr.minreq = static_cast<uint32_t>(-1);
  pulse_attr.fragsize =
      static_cast<uint32_t>(nyx::kCallAudioFrameSamples * sizeof(int16_t));
  for (const QAudioDevice& in_dev : inputCandidates(preferred_input_id_)) {
    if (in_dev.isNull()) continue;
    const QByteArray device_id = in_dev.id();
    int pulse_error = 0;
    pa_simple* capture = pa_simple_new(
        nullptr, "Nyx", PA_STREAM_RECORD,
        device_id.isEmpty() ? nullptr : device_id.constData(), "Voice capture",
        &pulse_spec, nullptr, &pulse_attr, &pulse_error);
    if (!capture) {
      NYX_AUDIO_LOG("openDevices: PulseAudio input failed id=%s error=%s",
                    device_id.constData(), pa_strerror(pulse_error));
      continue;
    }
    pulse_capture_ = capture;
    preferred_input_id_ = QString::fromUtf8(device_id);
    capture_rate_ = nyx::kCallAudioSampleRate;
    opened_in = true;
    NYX_AUDIO_LOG("openDevices: PulseAudio input ok desc=%s id=%s",
                  qPrintable(in_dev.description()), device_id.constData());
    break;
  }
#endif

  if (!opened_in) {
    for (const QAudioDevice& in_dev : inputCandidates(preferred_input_id_)) {
      if (in_dev.isNull()) continue;
      const QAudioFormat in_fmt = makeCaptureFormat(in_dev);
      if (in_fmt.sampleFormat() == QAudioFormat::Unknown) continue;
      auto src = std::make_unique<QAudioSource>(in_dev, in_fmt);
      const int bytes_20ms_in =
          in_fmt.bytesForDuration(nyx::kCallAudioFrameMs * 1000);
      src->setBufferSize(std::max(bytes_20ms_in * 10, 8192));
      QIODevice* dev = src->start();
      if (!dev) {
        NYX_AUDIO_LOG("openDevices: input failed id=%s",
                      qPrintable(QString::fromUtf8(in_dev.id())));
        continue;
      }
      source_ = std::move(src);
      source_dev_ = dev;
      preferred_input_id_ = QString::fromUtf8(in_dev.id());
      capture_rate_ = in_fmt.sampleRate();
      opened_in = true;
      NYX_AUDIO_LOG("openDevices: input ok desc=%s rate=%d ch=%d format=%d",
                    qPrintable(in_dev.description()), capture_rate_,
                    in_fmt.channelCount(),
                    static_cast<int>(in_fmt.sampleFormat()));
      break;
    }
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
    close_input();
    return false;
  }
  const QAudioFormat out_fmt = makeVoipFormat(out_dev);
  if (out_fmt.sampleFormat() != QAudioFormat::Int16) {
    close_input();
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
    close_input();
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
  if (!encoder_.ok()) {
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
    const auto [peer, pkt] = pending_remote_.front();
    pending_remote_.pop_front();
    onRemoteOpus(peer, pkt);
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
#if defined(NYX_HAS_PULSE_SIMPLE)
  if (pulse_capture_) {
    pa_simple_free(static_cast<pa_simple*>(pulse_capture_));
    pulse_capture_ = nullptr;
  }
#endif
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
  remote_peers_.clear();
  dominant_speaker_.clear();
  dominant_since_ms_ = 0;
  dominant_last_voice_ms_ = 0;
  local_voice_last_ms_ = 0;
  local_voice_level_.store(0, std::memory_order_release);
  if (local_voice_active_.exchange(false, std::memory_order_acq_rel)) {
    emit localVoiceActiveChanged(false);
  }
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
    double energy = 0.0;
    for (int i = 0; i < nyx::kCallAudioFrameSamples; ++i) {
      const double sample = static_cast<double>(opus_pcm_[static_cast<std::size_t>(i)]) /
                            32768.0;
      energy += sample * sample;
    }
    const float rms =
        static_cast<float>(std::sqrt(energy / nyx::kCallAudioFrameSamples));
    local_voice_level_.store(
        static_cast<uint8_t>(std::clamp(rms * 700.0f, 0.0f, 255.0f)),
        std::memory_order_release);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (rms >= 0.015f) local_voice_last_ms_ = now;
    const bool voice = now - local_voice_last_ms_ <= 300 &&
                       !muted_.load(std::memory_order_acquire);
    if (local_voice_active_.exchange(voice, std::memory_order_acq_rel) != voice) {
      emit localVoiceActiveChanged(voice);
    }
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
  if (!mic_test_.load(std::memory_order_acquire)) mixRemoteAudio();
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

#if defined(NYX_HAS_PULSE_SIMPLE)
  if (pulse_capture_) {
    std::array<int16_t, nyx::kCallAudioFrameSamples> pulse_pcm{};
    int pulse_error = 0;
    if (pa_simple_read(static_cast<pa_simple*>(pulse_capture_), pulse_pcm.data(),
                       pulse_pcm.size() * sizeof(int16_t), &pulse_error) < 0) {
      NYX_AUDIO_LOG("PulseAudio capture read failed: %s", pa_strerror(pulse_error));
      return;
    }
    pushCapturePcm(pulse_pcm.data(), static_cast<int>(pulse_pcm.size()));
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

void CallAudioIo::onRemoteOpus(const QString& peerId, const QByteArray& packet) {
  if (!ensureOnAudioThread("onRemoteOpus")) {
    QMetaObject::invokeMethod(this, [this, peerId, packet]() { onRemoteOpus(peerId, packet); },
                              Qt::QueuedConnection);
    return;
  }
  if (packet.isEmpty()) return;
  const QString key = peerId.isEmpty() ? QStringLiteral("direct") : peerId;
  if (!running_.load(std::memory_order_acquire)) {
    pending_remote_.emplace_back(key, packet);
    while (pending_remote_.size() > 80) pending_remote_.pop_front();
    return;
  }
  auto& peer = remote_peers_[key];
  if (!peer.decoder) peer.decoder = std::make_unique<nyx::OpusDecoderWrap>();
  if (!peer.decoder->ok()) return;
  auto pcm = peer.decoder->decode(reinterpret_cast<const uint8_t*>(packet.constData()),
                                  static_cast<std::size_t>(packet.size()));
  if (!pcm || pcm->empty()) {
    NYX_AUDIO_LOG("onRemoteOpus decode failed bytes=%d", int(packet.size()));
    return;
  }

  double energy = 0.0;
  for (int16_t sample : *pcm) {
    const double normalized = static_cast<double>(sample) / 32768.0;
    energy += normalized * normalized;
  }
  peer.level = static_cast<float>(std::sqrt(energy / static_cast<double>(pcm->size())));
  peer.last_packet_ms = QDateTime::currentMSecsSinceEpoch();
  peer.jitter.push_back(std::move(*pcm));
  while (peer.jitter.size() > 8) peer.jitter.pop_front();
}

void CallAudioIo::writePlayback(const std::vector<int16_t>& pcm) {
  if (pcm.empty()) return;
  if (use_android_voice_track_) {
    nyx_android::voice_playback_write(pcm.data(), static_cast<int>(pcm.size()));
    return;
  }
  if (!sink_dev_) return;
  if (playback_rate_ == nyx::kCallAudioSampleRate &&
      sink_->format().channelCount() == 1) {
    sink_dev_->write(reinterpret_cast<const char*>(pcm.data()),
                     static_cast<qint64>(pcm.size() * sizeof(int16_t)));
    return;
  }

  std::vector<int16_t> play;
  resampleMono(pcm.data(), static_cast<int>(pcm.size()), nyx::kCallAudioSampleRate, &play,
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

void CallAudioIo::mixRemoteAudio() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  std::vector<const std::vector<int16_t>*> frames;
  QString strongest;
  float strongest_level = 0.012f;

  for (auto it = remote_peers_.begin(); it != remote_peers_.end();) {
    auto& peer = it->second;
    if (now - peer.last_packet_ms > 3000) {
      it = remote_peers_.erase(it);
      continue;
    }
    if (peer.level > strongest_level && now - peer.last_packet_ms < 250) {
      strongest_level = peer.level;
      strongest = it->first;
    }
    if (!peer.primed && peer.jitter.size() >= 2) peer.primed = true;
    if (peer.primed && !peer.jitter.empty()) frames.push_back(&peer.jitter.front());
    ++it;
  }

  if (strongest == dominant_speaker_ && !strongest.isEmpty()) {
    dominant_last_voice_ms_ = now;
  } else if (strongest.isEmpty()) {
    if (!dominant_speaker_.isEmpty() && now - dominant_last_voice_ms_ >= 600) {
      dominant_speaker_.clear();
      dominant_since_ms_ = now;
      emit dominantSpeakerChanged({});
    }
  } else if (dominant_speaker_.isEmpty() || now - dominant_since_ms_ >= 350) {
      dominant_speaker_ = strongest;
      dominant_since_ms_ = now;
      dominant_last_voice_ms_ = now;
      emit dominantSpeakerChanged(dominant_speaker_);
  }

  if (frames.empty()) return;
  std::vector<int32_t> accum(static_cast<std::size_t>(nyx::kCallAudioFrameSamples), 0);
  for (const auto* frame : frames) {
    const std::size_t n = std::min(accum.size(), frame->size());
    for (std::size_t i = 0; i < n; ++i) accum[i] += (*frame)[i];
  }
  const double gain = 1.0 / std::sqrt(static_cast<double>(frames.size()));
  double peak = 1.0;
  for (int32_t sample : accum) peak = std::max(peak, std::abs(sample * gain));
  const double limiter = peak > 32000.0 ? 32000.0 / peak : 1.0;
  std::vector<int16_t> mixed(accum.size());
  for (std::size_t i = 0; i < accum.size(); ++i) {
    mixed[i] = static_cast<int16_t>(
        std::clamp(accum[i] * gain * limiter, -32768.0, 32767.0));
  }
  for (auto& [_, peer] : remote_peers_) {
    if (peer.primed && !peer.jitter.empty()) peer.jitter.pop_front();
  }
  writePlayback(mixed);
}

#else

CallAudioIo::CallAudioIo(QObject* parent) : QObject(parent) {}
CallAudioIo::~CallAudioIo() = default;
void CallAudioIo::setSendFn(SendFn) {}
bool CallAudioIo::start() { return false; }
void CallAudioIo::stop() {}
void CallAudioIo::setMuted(bool) {}
void CallAudioIo::onCaptureReady() {}
void CallAudioIo::onRemoteOpus(const QString&, const QByteArray&) {}
void CallAudioIo::mixRemoteAudio() {}
void CallAudioIo::writePlayback(const std::vector<int16_t>&) {}
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
