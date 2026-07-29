#include "call_video_io.hpp"
#include "android_platform.hpp"

#include "nyx/call_av1.hpp"
#include "nyx/call_media.hpp"

#include <QBuffer>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QCoreApplication>
#include <QDateTime>
#include <QImage>
#include <QMediaDevices>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#if !defined(Q_OS_ANDROID)
#include <QCamera>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QVideoSink>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(Q_OS_ANDROID)
#include <android/log.h>
#define NYX_VIDEO_LOG(...) __android_log_print(ANDROID_LOG_INFO, "NyxVideo", __VA_ARGS__)
#else
#define NYX_VIDEO_LOG(...)                                                                         \
  do {                                                                                             \
  } while (0)
#endif

#if defined(NYX_HAS_QT_MULTIMEDIA)

namespace {

QImage containFrame(const QImage& src, int w, int h) {
  if (src.isNull() || w <= 0 || h <= 0)
    return {};
  QImage scaled = src.convertToFormat(QImage::Format_RGB32)
                      .scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QImage out(w, h, QImage::Format_RGB32);
  out.fill(Qt::black);
  QPainter painter(&out);
  painter.drawImage((w - scaled.width()) / 2, (h - scaled.height()) / 2, scaled);
  return out;
}

QImage applyMetaOrientation(QImage img, const QVideoFrame& frame) {
  if (img.isNull())
    return img;
  switch (frame.rotationAngle()) {
  case QVideoFrame::Rotation90:
    img = img.transformed(QTransform().rotate(90), Qt::FastTransformation);
    break;
  case QVideoFrame::Rotation180:
    img = img.transformed(QTransform().rotate(180), Qt::FastTransformation);
    break;
  case QVideoFrame::Rotation270:
    img = img.transformed(QTransform().rotate(270), Qt::FastTransformation);
    break;
  default:
    break;
  }
  if (frame.mirrored())
    img = img.mirrored(true, false);
  return img;
}

/**
 * Many Android cameras deliver landscape buffers with rotationAngle=0.
 * Rotate so a portrait-held phone shows an upright person.
 */
QImage uprightForDevice(QImage img, bool front_camera) {
  if (img.isNull())
    return img;
#if defined(Q_OS_ANDROID)
  if (img.width() > img.height()) {
    const int deg = front_camera ? 270 : 90;
    img = img.transformed(QTransform().rotate(deg), Qt::FastTransformation);
  }
#else
  Q_UNUSED(front_camera);
#endif
  return img;
}

#if defined(Q_OS_ANDROID)
bool isCpuFriendly(QVideoFrameFormat::PixelFormat fmt) {
  switch (fmt) {
  case QVideoFrameFormat::Format_ARGB8888:
  case QVideoFrameFormat::Format_XRGB8888:
  case QVideoFrameFormat::Format_BGRA8888:
  case QVideoFrameFormat::Format_BGRX8888:
  case QVideoFrameFormat::Format_ABGR8888:
  case QVideoFrameFormat::Format_RGBA8888:
  case QVideoFrameFormat::Format_RGBX8888:
  case QVideoFrameFormat::Format_AYUV:
  case QVideoFrameFormat::Format_YUV420P:
  case QVideoFrameFormat::Format_YUV422P:
  case QVideoFrameFormat::Format_NV12:
  case QVideoFrameFormat::Format_NV21:
  case QVideoFrameFormat::Format_YUYV:
  case QVideoFrameFormat::Format_UYVY:
  case QVideoFrameFormat::Format_IMC1:
  case QVideoFrameFormat::Format_IMC2:
  case QVideoFrameFormat::Format_IMC3:
  case QVideoFrameFormat::Format_IMC4:
  case QVideoFrameFormat::Format_Y8:
  case QVideoFrameFormat::Format_Y16:
    return true;
  default:
    return false;
  }
}
#endif

/** Prefer CPU-friendly conversion; deep-copy while frame bits are still valid. */
QImage frameToImage(QVideoFrame frame) {
  if (!frame.isValid())
    return {};

  // Fast path: already a CPU image (common on desktop).
  {
    QImage img = frame.toImage();
    if (!img.isNull())
      return img.copy();
  }

  if (!frame.map(QVideoFrame::ReadOnly))
    return {};
  QImage img = frame.toImage();
  if (img.isNull()) {
    // Build from plane 0 when toImage fails but map succeeded (some Android paths).
    const QVideoFrameFormat fmt = frame.surfaceFormat();
    const int w = frame.width();
    const int h = frame.height();
    if (w > 0 && h > 0 && frame.planeCount() > 0) {
      const uchar* bits = frame.bits(0);
      const int bpl = frame.bytesPerLine(0);
      if (bits && bpl > 0) {
        QImage::Format qfmt = QImage::Format_Invalid;
        switch (fmt.pixelFormat()) {
        case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRX8888:
          qfmt = QImage::Format_RGB32;
          break;
        case QVideoFrameFormat::Format_ARGB8888:
        case QVideoFrameFormat::Format_XRGB8888:
          qfmt = QImage::Format_ARGB32;
          break;
        case QVideoFrameFormat::Format_RGBA8888:
        case QVideoFrameFormat::Format_RGBX8888:
          qfmt = QImage::Format_RGBA8888;
          break;
        default:
          break;
        }
        if (qfmt != QImage::Format_Invalid)
          img = QImage(bits, w, h, bpl, qfmt).copy();
      }
    }
  } else {
    img = img.copy(); // deep copy while mapped
  }
  frame.unmap();
  return img;
}

QCameraFormat bestFormat(const QCameraDevice& device, int target_w, int target_h) {
#if defined(Q_OS_ANDROID)
  Q_UNUSED(device);
  Q_UNUSED(target_w);
  Q_UNUSED(target_h);
  return {};
#else
  QCameraFormat best;
  int best_score = -1;
  for (const QCameraFormat& f : device.videoFormats()) {
    if (f.resolution().width() < 160 || f.resolution().height() < 120)
      continue;
    const int dw = std::abs(f.resolution().width() - target_w * 2);
    const int dh = std::abs(f.resolution().height() - target_h * 2);
    const int area = f.resolution().width() * f.resolution().height();
    const int fps = static_cast<int>(f.maxFrameRate());
    int score = 100000 - dw - dh + std::min(fps, 24) * 10;
    if (area > 1280 * 720)
      score -= (area - 1280 * 720) / 2000;
    if (score > best_score) {
      best_score = score;
      best = f;
    }
  }
  return best;
#endif
}

QString cameraLabel(const QCameraDevice& d) {
  QString name = d.description();
  if (name.isEmpty())
    name = QString::fromUtf8(d.id());
  switch (d.position()) {
  case QCameraDevice::FrontFace:
    return QStringLiteral("%1 (передняя)").arg(name);
  case QCameraDevice::BackFace:
    return QStringLiteral("%1 (задняя)").arg(name);
  default:
    return name;
  }
}

QImage makeBlackFrame(int w, int h) {
  QImage img(std::max(2, w), std::max(2, h), QImage::Format_RGB32);
  img.fill(Qt::black);
  return img;
}

std::vector<uint8_t> imageToI420(const QImage& source, int width, int height) {
  QImage img = source.convertToFormat(QImage::Format_RGB32);
  if (img.width() != width || img.height() != height)
    img = img.scaled(width, height, Qt::IgnoreAspectRatio, Qt::FastTransformation);
  if (img.isNull() || (width & 1) || (height & 1))
    return {};

  const int y_size = width * height;
  const int uv_width = width / 2;
  const int uv_size = uv_width * (height / 2);
  std::vector<uint8_t> out(static_cast<std::size_t>(y_size + uv_size * 2));
  uint8_t* y_plane = out.data();
  uint8_t* u_plane = y_plane + y_size;
  uint8_t* v_plane = u_plane + uv_size;

  for (int y = 0; y < height; y += 2) {
    const auto* row0 = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    const auto* row1 = reinterpret_cast<const QRgb*>(img.constScanLine(y + 1));
    for (int x = 0; x < width; x += 2) {
      int r_sum = 0, g_sum = 0, b_sum = 0;
      const QRgb pixels[4] = {row0[x], row0[x + 1], row1[x], row1[x + 1]};
      for (int i = 0; i < 4; ++i) {
        const int r = qRed(pixels[i]);
        const int g = qGreen(pixels[i]);
        const int b = qBlue(pixels[i]);
        const int py = std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255);
        const int ox = x + (i & 1);
        const int oy = y + (i >> 1);
        y_plane[oy * width + ox] = static_cast<uint8_t>(py);
        r_sum += r;
        g_sum += g;
        b_sum += b;
      }
      const int r = r_sum / 4;
      const int g = g_sum / 4;
      const int b = b_sum / 4;
      const int uv = (y / 2) * uv_width + x / 2;
      u_plane[uv] =
          static_cast<uint8_t>(std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
      v_plane[uv] =
          static_cast<uint8_t>(std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
    }
  }
  return out;
}

QImage i420ToImage(const nyx::Av1Decoder::Frame& frame) {
  if (frame.width <= 0 || frame.height <= 0 || (frame.width & 1) || (frame.height & 1))
    return {};
  const int y_size = frame.width * frame.height;
  const int uv_width = frame.width / 2;
  const int uv_size = uv_width * (frame.height / 2);
  if (frame.i420.size() < static_cast<std::size_t>(y_size + uv_size * 2))
    return {};
  const uint8_t* yp = frame.i420.data();
  const uint8_t* up = yp + y_size;
  const uint8_t* vp = up + uv_size;
  QImage out(frame.width, frame.height, QImage::Format_RGB32);
  if (out.isNull())
    return {};
  for (int y = 0; y < frame.height; ++y) {
    auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
    for (int x = 0; x < frame.width; ++x) {
      const int c = std::max(0, static_cast<int>(yp[y * frame.width + x]) - 16);
      const int d = static_cast<int>(up[(y / 2) * uv_width + x / 2]) - 128;
      const int e = static_cast<int>(vp[(y / 2) * uv_width + x / 2]) - 128;
      const int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
      const int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
      const int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);
      dst[x] = qRgb(r, g, b);
    }
  }
  return out;
}

} // namespace

int CallVideoIo::encodeWidth() const {
  return nyx::kCallVideoWidth;
}

int CallVideoIo::encodeHeight() const {
  return nyx::kCallVideoHeight;
}

int CallVideoIo::encodeFps() const {
  return nyx::kCallVideoFps;
}

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {
  encode_timer_ = new QTimer(this);
  encode_timer_->setInterval(1000 / std::max(1, encodeFps()));
  connect(encode_timer_, &QTimer::timeout, this, &CallVideoIo::onEncodeTick);
#if defined(Q_OS_ANDROID)
  nyx_android::set_native_camera_callbacks(
      [](const QByteArray& jpeg, bool front, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeJpeg(jpeg, front);
      },
      [](const QString& msg, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeCameraError(msg);
      },
      [](bool front, const QString& id, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeCameraStarted(front, id);
      },
      this);
#endif
}

CallVideoIo::~CallVideoIo() {
  stop();
}

bool CallVideoIo::ensureOnVideoThread(const char* where) {
  if (QThread::currentThread() == thread())
    return true;
  NYX_VIDEO_LOG("marshal %s → video thread", where ? where : "?");
  Q_UNUSED(where);
  return false;
}

void CallVideoIo::setSendFn(SendFn fn) {
  if (!ensureOnVideoThread("setSendFn")) {
    QMetaObject::invokeMethod(
        this,
        [this, fn = std::move(fn)]() mutable { setSendFn(std::move(fn)); },
        Qt::QueuedConnection);
    return;
  }
  send_fn_ = std::move(fn);
}

QString CallVideoIo::preferredCameraId() const {
  QMutexLocker lock(&frames_mutex_);
  return preferred_camera_id_;
}

QString CallVideoIo::activeCameraId() const {
  QMutexLocker lock(&frames_mutex_);
  return camera_id_;
}

QString CallVideoIo::focusedPeerId() const {
  QMutexLocker lock(&frames_mutex_);
  return focused_peer_;
}

QImage CallVideoIo::lastRemoteFrame() const {
  QMutexLocker lock(&frames_mutex_);
  return remote_;
}

QImage CallVideoIo::lastLocalFrame() const {
  QMutexLocker lock(&frames_mutex_);
  return local_;
}

QVariantList CallVideoIo::listCameraDevices() {
  QVariantList out;
  for (const QCameraDevice& d : QMediaDevices::videoInputs()) {
    QVariantMap row;
    row.insert(QStringLiteral("id"), QString::fromUtf8(d.id()));
    row.insert(QStringLiteral("text"), cameraLabel(d));
    row.insert(QStringLiteral("position"), static_cast<int>(d.position()));
    out.append(row);
  }
  return out;
}

QCameraDevice CallVideoIo::resolveCameraDevice() const {
  const auto cams = QMediaDevices::videoInputs();
  if (cams.isEmpty())
    return {};
  if (!preferred_camera_id_.isEmpty()) {
    for (const auto& c : cams) {
      if (QString::fromUtf8(c.id()) == preferred_camera_id_)
        return c;
    }
  }
  for (const auto& c : cams) {
    if (c.position() == QCameraDevice::FrontFace)
      return c;
  }
  return cams.front();
}

#if !defined(Q_OS_ANDROID)
void CallVideoIo::handleCameraFrame(const QVideoFrame& frame) {
  if (!running_.load(std::memory_order_acquire))
    return;
  if (!capturing_.load(std::memory_order_acquire))
    return;
  if (!camera_enabled_.load(std::memory_order_acquire))
    return;
  if (!frame.isValid())
    return;
  if (ingest_busy_.exchange(true, std::memory_order_acq_rel))
    return;

  QImage img = uprightForDevice(applyMetaOrientation(frameToImage(frame), frame),
                                front_camera_.load(std::memory_order_relaxed));
  if (img.isNull()) {
    ingest_busy_.store(false, std::memory_order_release);
    return;
  }
  QImage cropped = containFrame(img, encodeWidth(), encodeHeight());
  if (cropped.isNull()) {
    ingest_busy_.store(false, std::memory_order_release);
    return;
  }

  const bool ok = QMetaObject::invokeMethod(
      this,
      [this, cropped = std::move(cropped)]() mutable {
        ingest_busy_.store(false, std::memory_order_release);
        ingestCapturedFrame(std::move(cropped));
      },
      Qt::QueuedConnection);
  if (!ok)
    ingest_busy_.store(false, std::memory_order_release);
}

void CallVideoIo::wireVideoSink() {
  if (!sink_ || sink_wired_)
    return;
  sink_wired_ = true;
  connect(
      sink_,
      &QVideoSink::videoFrameChanged,
      this,
      [this](const QVideoFrame& frame) { handleCameraFrame(frame); },
      Qt::QueuedConnection);
}

bool CallVideoIo::openCamera(const QCameraDevice& device) {
  if (device.isNull())
    return false;
  const QString new_id = QString::fromUtf8(device.id());
  if (capturing_.load(std::memory_order_acquire) && camera_ && new_id == camera_id_)
    return true;

  closeCameraHardware();

  if (!session_)
    session_ = std::make_unique<QMediaCaptureSession>();
  if (!sink_) {
    sink_ = new QVideoSink(this);
    sink_wired_ = false;
  }
  wireVideoSink();

  camera_ = std::make_unique<QCamera>(device);
  const QCameraFormat fmt = bestFormat(device, encodeWidth(), encodeHeight());
  if (!fmt.isNull())
    camera_->setCameraFormat(fmt);

  session_->setVideoSink(sink_);
  session_->setCamera(camera_.get());
  camera_id_ = new_id;
  preferred_camera_id_ = camera_id_;
  front_camera_.store(device.position() == QCameraDevice::FrontFace, std::memory_order_relaxed);
  const auto cams = QMediaDevices::videoInputs();
  camera_index_ = 0;
  for (int i = 0; i < cams.size(); ++i) {
    if (QString::fromUtf8(cams.at(i).id()) == camera_id_) {
      camera_index_ = i;
      break;
    }
  }
  connect(camera_.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& err) {
    NYX_VIDEO_LOG("camera error: %s", qPrintable(err));
    closeCameraHardware();
    capturing_.store(false, std::memory_order_release);
    camera_enabled_.store(false, std::memory_order_release);
    emit cameraChanged();
    emit cameraOpenFailed();
  });
  NYX_VIDEO_LOG(
      "camera_->start() id=%s front=%d", qPrintable(camera_id_), front_camera_.load() ? 1 : 0);
  camera_->start();
  capturing_.store(true, std::memory_order_release);
  emit cameraChanged();
  return true;
}

void CallVideoIo::closeCameraHardware() {
  capturing_.store(false, std::memory_order_release);
  if (session_) {
    session_->setCamera(nullptr);
    session_->setVideoSink(nullptr);
  }
  if (camera_) {
    camera_->stop();
    camera_.reset();
  }
  {
    QMutexLocker lock(&frames_mutex_);
    pending_ = QImage();
  }
}
#else
void CallVideoIo::handleCameraFrame(const QVideoFrame&) {}
void CallVideoIo::wireVideoSink() {}

bool CallVideoIo::openCamera(const QCameraDevice& device) {
  // Qt Multimedia Camera2 ANRs / freezes UI on this device family.
  // Native Camera2 + ImageReader runs on nyx-camera2 HandlerThread — no SurfaceView.
  Q_UNUSED(device);
  NYX_VIDEO_LOG("openCamera native prefer_front=%d thr=%p gui=%p",
                front_camera_.load() ? 1 : 0,
                static_cast<void*>(QThread::currentThread()),
                static_cast<void*>(QCoreApplication::instance()
                                       ? QCoreApplication::instance()->thread()
                                       : nullptr));
  const bool prefer_front =
      preferred_camera_id_.isEmpty() ? true : front_camera_.load(std::memory_order_relaxed);
  // If preferred id looks like back, use back.
  bool front = prefer_front;
  if (!device.isNull()) {
    front = device.position() != QCameraDevice::BackFace;
  }
  front_camera_.store(front, std::memory_order_relaxed);
  last_ingest_ms_.store(0, std::memory_order_relaxed);
  capturing_.store(true, std::memory_order_release); // optimistic; cleared on error
  nyx_android::native_camera_start(front);
  emit cameraChanged();

  const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
  QTimer::singleShot(4000, this, [this, t0]() {
    if (!running_.load(std::memory_order_acquire))
      return;
    if (!camera_enabled_.load(std::memory_order_acquire))
      return;
    const qint64 last = last_ingest_ms_.load(std::memory_order_relaxed);
    if (last > 0) {
      NYX_VIDEO_LOG("native camera watchdog OK last_ingest=%lld age=%lld",
                    static_cast<long long>(last),
                    static_cast<long long>(QDateTime::currentMSecsSinceEpoch() - t0));
      return;
    }
    NYX_VIDEO_LOG("native camera watchdog FAIL no frames age=%lld",
                  static_cast<long long>(QDateTime::currentMSecsSinceEpoch() - t0));
    closeCameraHardware();
    camera_enabled_.store(false, std::memory_order_release);
    emit cameraOpenFailed();
    emit cameraChanged();
  });
  return true;
}

void CallVideoIo::closeCameraHardware() {
  NYX_VIDEO_LOG("closeCameraHardware native");
  capturing_.store(false, std::memory_order_release);
  nyx_android::native_camera_stop();
  {
    QMutexLocker lock(&frames_mutex_);
    pending_ = QImage();
  }
}

void CallVideoIo::onNativeJpeg(const QByteArray& jpeg, bool front) {
  if (!running_.load(std::memory_order_acquire))
    return;
  if (!camera_enabled_.load(std::memory_order_acquire))
    return;
  if (jpeg.isEmpty())
    return;
  if (ingest_busy_.exchange(true, std::memory_order_acq_rel))
    return;
  front_camera_.store(front, std::memory_order_relaxed);
  QImage img;
  if (!img.loadFromData(jpeg, "JPG")) {
    ingest_busy_.store(false, std::memory_order_release);
    NYX_VIDEO_LOG("native jpeg decode failed bytes=%d", int(jpeg.size()));
    return;
  }
  img = uprightForDevice(img, front);
  QImage cropped = containFrame(img, encodeWidth(), encodeHeight());
  ingest_busy_.store(false, std::memory_order_release);
  if (cropped.isNull())
    return;
  NYX_VIDEO_LOG("native jpeg ok %dx%d -> %dx%d bytes=%d",
                img.width(),
                img.height(),
                cropped.width(),
                cropped.height(),
                int(jpeg.size()));
  ingestCapturedFrame(std::move(cropped));
}

void CallVideoIo::onNativeCameraError(const QString& message) {
  NYX_VIDEO_LOG("native camera error: %s", qPrintable(message));
  closeCameraHardware();
  camera_enabled_.store(false, std::memory_order_release);
  emit cameraOpenFailed();
  emit cameraChanged();
}

void CallVideoIo::onNativeCameraStarted(bool front, const QString& cameraId) {
  NYX_VIDEO_LOG("native camera started front=%d id=%s", front ? 1 : 0, qPrintable(cameraId));
  front_camera_.store(front, std::memory_order_relaxed);
  camera_id_ = cameraId;
  preferred_camera_id_ = cameraId;
  capturing_.store(true, std::memory_order_release);
  emit cameraChanged();
}
#endif

void CallVideoIo::setPreferredCameraId(const QString& id) {
  if (!ensureOnVideoThread("setPreferredCameraId")) {
    QMetaObject::invokeMethod(
        this, [this, id]() { setPreferredCameraId(id); }, Qt::QueuedConnection);
    return;
  }
  preferred_camera_id_ = id.trimmed();
  if (running_.load(std::memory_order_acquire) && capturing_.load(std::memory_order_acquire) &&
      !preferred_camera_id_.isEmpty()) {
    const QCameraDevice d = resolveCameraDevice();
    if (!d.isNull() && QString::fromUtf8(d.id()) != camera_id_)
      openCamera(d);
  }
  emit cameraChanged();
}

bool CallVideoIo::start() {
  if (!ensureOnVideoThread("start")) {
    QMetaObject::invokeMethod(this, [this]() { start(); }, Qt::QueuedConnection);
    return true;
  }
  if (running_.load(std::memory_order_acquire))
    return true;
  running_.store(true, std::memory_order_release);
#if defined(Q_OS_ANDROID)
  nyx_android::set_native_camera_callbacks(
      [](const QByteArray& jpeg, bool front, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeJpeg(jpeg, front);
      },
      [](const QString& msg, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeCameraError(msg);
      },
      [](bool front, const QString& id, void* ctx) {
        static_cast<CallVideoIo*>(ctx)->onNativeCameraStarted(front, id);
      },
      this);
#endif

  {
    QMutexLocker lock(&frames_mutex_);
    peers_.clear();
    focused_peer_.clear();
    remote_ = QImage();
    local_ = QImage();
    pending_ = QImage();
  }
  frame_id_ = 0;
  encoder_ = std::make_unique<nyx::Av1Encoder>();
  if (!encoder_->ok()) {
    encoder_.reset();
    running_.store(false, std::memory_order_release);
    NYX_VIDEO_LOG("start failed: AV1 encoder unavailable");
    return false;
  }
  capturing_.store(false, std::memory_order_release);
  local_dirty_ = false;
  ingest_busy_.store(false, std::memory_order_release);
  last_ingest_ms_.store(0, std::memory_order_relaxed);
  encode_busy_ = false;
  if (encode_timer_)
    encode_timer_->setInterval(1000 / std::max(1, encodeFps()));

  if (camera_enabled_.load(std::memory_order_acquire)) {
#if defined(Q_OS_ANDROID)
    // Defer Camera2 so Answer UI / ringtone stop paint first.
    QTimer::singleShot(350, this, [this]() {
      if (!running_.load(std::memory_order_acquire))
        return;
      if (!camera_enabled_.load(std::memory_order_acquire))
        return;
      if (capturing_.load(std::memory_order_acquire))
        return;
      const QCameraDevice device = resolveCameraDevice();
      if (device.isNull()) {
        NYX_VIDEO_LOG("start deferred: no device");
        emit cameraOpenFailed();
        return;
      }
      if (!openCamera(device)) {
        capturing_.store(false, std::memory_order_release);
        emit cameraOpenFailed();
      }
    });
#else
    const QCameraDevice device = resolveCameraDevice();
    if (!device.isNull()) {
      if (!openCamera(device))
        capturing_.store(false, std::memory_order_release);
    }
#endif
  }
  if (encode_timer_)
    encode_timer_->start();
  NYX_VIDEO_LOG(
      "start ok camera_enabled=%d capturing=%d", camera_enabled_.load(), capturing_.load());
  return true;
}

void CallVideoIo::stop() {
  if (!ensureOnVideoThread("stop")) {
    QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
    return;
  }
  NYX_VIDEO_LOG("stop");
  running_.store(false, std::memory_order_release);
  local_dirty_ = false;
  if (encode_timer_)
    encode_timer_->stop();
  closeCameraHardware();
  encoder_.reset();
#if !defined(Q_OS_ANDROID)
  if (sink_)
    disconnect(sink_, nullptr, this, nullptr);
  sink_wired_ = false;
  session_.reset();
  if (sink_) {
    sink_->deleteLater();
    sink_ = nullptr;
  }
#endif
#if defined(Q_OS_ANDROID)
  nyx_android::set_native_camera_callbacks(nullptr, nullptr, nullptr, nullptr);
#endif
  {
    QMutexLocker lock(&frames_mutex_);
    pending_ = QImage();
    local_ = QImage();
    remote_ = QImage();
    peers_.clear();
    focused_peer_.clear();
  }
  emit cameraChanged();
  emit videoPeersChanged();
  emit localFrameChanged();
  emit remoteFrameChanged(QString());
  camera_index_ = 0;
  front_camera_.store(false, std::memory_order_relaxed);
  ingest_busy_.store(false, std::memory_order_release);
  last_ingest_ms_.store(0, std::memory_order_relaxed);
}

bool CallVideoIo::canSwitchCamera() const {
#if defined(Q_OS_ANDROID)
  return nyx_android::native_camera_has_front_and_back();
#else
  const auto cams = QMediaDevices::videoInputs();
  if (cams.size() > 1)
    return true;
  bool has_front = false, has_back = false;
  for (const auto& c : cams) {
    if (c.position() == QCameraDevice::FrontFace)
      has_front = true;
    if (c.position() == QCameraDevice::BackFace)
      has_back = true;
  }
  return has_front && has_back;
#endif
}

bool CallVideoIo::switchCamera() {
  if (!ensureOnVideoThread("switchCamera")) {
    QMetaObject::invokeMethod(this, [this]() { switchCamera(); }, Qt::QueuedConnection);
    return true;
  }
  if (!running_.load(std::memory_order_acquire) || !capturing_.load(std::memory_order_acquire))
    return false;
#if defined(Q_OS_ANDROID)
  NYX_VIDEO_LOG("switchCamera native");
  nyx_android::native_camera_switch_facing();
  return true;
#else
  const auto cams = QMediaDevices::videoInputs();
  if (cams.isEmpty())
    return false;

  const bool front = front_camera_.load(std::memory_order_relaxed);
  const QCameraDevice::Position want = front ? QCameraDevice::BackFace : QCameraDevice::FrontFace;
  for (int i = 0; i < cams.size(); ++i) {
    if (cams.at(i).position() == want) {
      camera_index_ = i;
      return openCamera(cams.at(i));
    }
  }
  if (cams.size() < 2)
    return false;
  camera_index_ = (camera_index_ + 1) % cams.size();
  return openCamera(cams.at(camera_index_));
#endif
}

void CallVideoIo::setCameraEnabled(bool on) {
  if (!ensureOnVideoThread("setCameraEnabled")) {
    QMetaObject::invokeMethod(this, [this, on]() { setCameraEnabled(on); }, Qt::QueuedConnection);
    return;
  }
  NYX_VIDEO_LOG("setCameraEnabled %d", on ? 1 : 0);
  camera_enabled_.store(on, std::memory_order_release);
  if (!running_.load(std::memory_order_acquire)) {
    emit cameraChanged();
    return;
  }
  if (!on) {
    closeCameraHardware();
    {
      QMutexLocker lock(&frames_mutex_);
      local_ = QImage();
      pending_ = makeBlackFrame(encodeWidth(), encodeHeight());
      local_dirty_ = true;
    }
    emit localFrameChanged();
    emit cameraChanged();
    return;
  }
  emit cameraChanged();
  // Defer open so the UI click handler returns before Camera2 negotiates.
  QTimer::singleShot(50, this, [this]() {
    if (!running_.load(std::memory_order_acquire))
      return;
    if (!camera_enabled_.load(std::memory_order_acquire))
      return;
    if (capturing_.load(std::memory_order_acquire))
      return;
    const QCameraDevice device = resolveCameraDevice();
    if (device.isNull()) {
      NYX_VIDEO_LOG("setCameraEnabled: no device");
      emit cameraOpenFailed();
      return;
    }
    if (!openCamera(device)) {
      capturing_.store(false, std::memory_order_release);
      emit cameraOpenFailed();
    }
  });
}

void CallVideoIo::setFocusedPeerId(const QString& peerId) {
  if (!ensureOnVideoThread("setFocusedPeerId")) {
    QMetaObject::invokeMethod(
        this, [this, peerId]() { setFocusedPeerId(peerId); }, Qt::QueuedConnection);
    return;
  }
  QMutexLocker lock(&frames_mutex_);
  focused_peer_ = peerId;
  remote_ = QImage();
  if (peerId.isEmpty())
    return;
  const auto it = peers_.find(peerId.toStdString());
  if (it != peers_.end())
    remote_ = it->second.frame;
}

QImage CallVideoIo::peerFrame(const QString& peerId) const {
  QMutexLocker lock(&frames_mutex_);
  if (peerId.isEmpty() || peerId == QLatin1String("direct")) {
    if (!remote_.isNull())
      return remote_;
    if (!focused_peer_.isEmpty()) {
      const auto it = peers_.find(focused_peer_.toStdString());
      if (it != peers_.end())
        return it->second.frame;
    }
    if (!peers_.empty())
      return peers_.cbegin()->second.frame;
    return {};
  }
  const auto it = peers_.find(peerId.toStdString());
  if (it != peers_.end())
    return it->second.frame;
  return {};
}

QStringList CallVideoIo::videoPeerIds() const {
  QMutexLocker lock(&frames_mutex_);
  QStringList out;
  out.reserve(static_cast<int>(peers_.size()));
  for (const auto& [id, _] : peers_)
    out.append(QString::fromStdString(id));
  out.sort();
  return out;
}

CallVideoIo::PeerDecoder& CallVideoIo::peerDecoder(const QString& peerId) {
  // Caller must hold frames_mutex_. Do NOT emit signals here (non-recursive mutex).
  const std::string key = peerId.toStdString();
  auto it = peers_.find(key);
  if (it != peers_.end())
    return it->second;
  PeerDecoder& slot = peers_[key];
  slot.reasm = std::make_unique<nyx::CallVideoReassembler>();
  slot.decoder = std::make_unique<nyx::Av1Decoder>();
  if (focused_peer_.isEmpty())
    focused_peer_ = peerId;
  return slot;
}

void CallVideoIo::ingestCapturedFrame(QImage cropped) {
  if (!running_.load(std::memory_order_acquire) ||
      !camera_enabled_.load(std::memory_order_acquire) || cropped.isNull())
    return;
  last_ingest_ms_.store(QDateTime::currentMSecsSinceEpoch(), std::memory_order_relaxed);
  QMutexLocker lock(&frames_mutex_);
  if (front_camera_.load(std::memory_order_relaxed)) {
    local_ = cropped.mirrored(true, false);
    if (pending_.isNull())
      pending_ = cropped;
  } else {
    local_ = cropped.copy();
    if (pending_.isNull())
      pending_ = std::move(cropped);
  }
  local_dirty_ = true;
}

void CallVideoIo::onEncodeTick() {
  if (!running_.load(std::memory_order_acquire))
    return;
  if (encode_busy_)
    return; // previous tick still encoding — skip rather than stall
  encode_busy_ = true;

  QImage frame;
  bool emit_local = false;
  const bool cam_on = camera_enabled_.load(std::memory_order_acquire);
  {
    QMutexLocker lock(&frames_mutex_);
    if (local_dirty_) {
      local_dirty_ = false;
      emit_local = true;
    }
    if (send_fn_ && transmit_enabled_.load(std::memory_order_acquire)) {
      if (!cam_on) {
        static qint64 s_last_black_ms = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - s_last_black_ms >= 500) {
          s_last_black_ms = now;
          frame = makeBlackFrame(encodeWidth(), encodeHeight());
        }
      } else if (capturing_.load(std::memory_order_acquire) && !pending_.isNull()) {
        frame = pending_;
        pending_ = QImage();
      }
    }
  }
  if (emit_local)
    emit localFrameChanged();
  if (frame.isNull() || !send_fn_) {
    encode_busy_ = false;
    return;
  }

  if (!encoder_) {
    encode_busy_ = false;
    return;
  }

  auto i420 = imageToI420(frame, encodeWidth(), encodeHeight());
  if (i420.empty()) {
    encode_busy_ = false;
    return;
  }
  const bool keyframe = force_keyframe_.exchange(false, std::memory_order_acq_rel) ||
                        (frame_id_ % static_cast<uint16_t>(encodeFps())) == 0;
  auto encoded = encoder_->encode_i420(i420.data(), encodeWidth(), encodeHeight(), keyframe);
  if (!encoded || encoded->empty()) {
    encode_busy_ = false;
    return;
  }
  const uint16_t fid = frame_id_++;
  auto frags = nyx::fragment_av1_frame(fid, keyframe, *encoded, nyx::kMaxCallMediaPayload);
  if (frags.empty()) {
    encode_busy_ = false;
    return;
  }
  int sent = 0;
  for (auto& frag : frags) {
    QByteArray bytes(reinterpret_cast<const char*>(frag.data()), static_cast<int>(frag.size()));
    if (!send_fn_(bytes))
      break;
    ++sent;
  }
  if (sent != static_cast<int>(frags.size())) {
    NYX_VIDEO_LOG("AV1 send drop fid=%u bytes=%d frags=%d sent=%d key=%d",
                  unsigned(fid),
                  int(encoded->size()),
                  int(frags.size()),
                  sent,
                  keyframe ? 1 : 0);
  } else {
    NYX_VIDEO_LOG("AV1 send fid=%u bytes=%d frags=%d key=%d",
                  unsigned(fid),
                  int(encoded->size()),
                  int(frags.size()),
                  keyframe ? 1 : 0);
  }
  encode_busy_ = false;
}

void CallVideoIo::onRemoteVideo(const QString& peerId, const QByteArray& frag_payload) {
  if (!ensureOnVideoThread("onRemoteVideo")) {
    QMetaObject::invokeMethod(
        this,
        [this, peerId, frag_payload]() { onRemoteVideo(peerId, frag_payload); },
        Qt::QueuedConnection);
    return;
  }
  if (frag_payload.isEmpty())
    return;
  const QString pid = peerId.isEmpty() ? QStringLiteral("direct") : peerId;

  bool peers_changed = false;
  nyx::ByteBuffer av1;
  nyx::Av1Decoder* decoder = nullptr;
  {
    QMutexLocker lock(&frames_mutex_);
    const auto before = peers_.size();
    auto& peer = peerDecoder(pid);
    peers_changed = peers_.size() != before;
    if (!peer.reasm || !peer.decoder || !peer.decoder->ok())
      return;

    nyx::ByteBuffer buf(frag_payload.begin(), frag_payload.end());
    auto full = peer.reasm->push(buf);
    if (!full) {
      lock.unlock();
      if (peers_changed)
        emit videoPeersChanged();
      return;
    }
    av1 = std::move(full->data);
    decoder = peer.decoder.get();
  }

  auto decoded = decoder->decode(av1.data(), av1.size());
  if (!decoded) {
    if (peers_changed)
      emit videoPeersChanged();
    return;
  }
  QImage rgb = i420ToImage(*decoded);
  if (rgb.isNull()) {
    if (peers_changed)
      emit videoPeersChanged();
    return;
  }

  QImage ready;
  {
    QMutexLocker lock(&frames_mutex_);
    auto& peer = peerDecoder(pid);
    peer.frame = rgb;
    if (focused_peer_.isEmpty() || focused_peer_ == pid) {
      focused_peer_ = pid;
      remote_ = peer.frame;
    }
    ready = peer.frame;
  }
  if (peers_changed)
    emit videoPeersChanged();
  if (!ready.isNull())
    emit remoteFrameChanged(pid);
}

#else

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {}
CallVideoIo::~CallVideoIo() = default;
void CallVideoIo::setSendFn(SendFn) {}
bool CallVideoIo::start() {
  return false;
}
void CallVideoIo::stop() {}
void CallVideoIo::onEncodeTick() {}
void CallVideoIo::ingestCapturedFrame(QImage) {}
void CallVideoIo::onRemoteVideo(const QString&, const QByteArray&) {}
bool CallVideoIo::canSwitchCamera() const {
  return false;
}
bool CallVideoIo::switchCamera() {
  return false;
}
void CallVideoIo::setCameraEnabled(bool) {}
void CallVideoIo::setFocusedPeerId(const QString&) {}
QStringList CallVideoIo::videoPeerIds() const {
  return {};
}
void CallVideoIo::setPreferredCameraId(const QString&) {}
QString CallVideoIo::preferredCameraId() const {
  return {};
}
QString CallVideoIo::activeCameraId() const {
  return {};
}
QString CallVideoIo::focusedPeerId() const {
  return {};
}
QImage CallVideoIo::lastLocalFrame() const {
  return {};
}
QImage CallVideoIo::lastRemoteFrame() const {
  return {};
}
QVariantList CallVideoIo::listCameraDevices() {
  return {};
}
QImage CallVideoIo::peerFrame(const QString&) const {
  return {};
}
CallVideoIo::PeerDecoder& CallVideoIo::peerDecoder(const QString&) {
  static PeerDecoder dummy;
  return dummy;
}
bool CallVideoIo::openCamera(const QCameraDevice&) {
  return false;
}
void CallVideoIo::closeCameraHardware() {}
void CallVideoIo::wireVideoSink() {}
void CallVideoIo::handleCameraFrame(const QVideoFrame&) {}
bool CallVideoIo::ensureOnVideoThread(const char*) {
  return true;
}
QCameraDevice CallVideoIo::resolveCameraDevice() const {
  return {};
}
int CallVideoIo::encodeWidth() const {
  return 320;
}
int CallVideoIo::encodeHeight() const {
  return 180;
}
int CallVideoIo::encodeFps() const {
  return 10;
}

#endif
