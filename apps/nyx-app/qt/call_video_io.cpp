#include "call_video_io.hpp"

#include "nyx/call_av1.hpp"
#include "nyx/call_media.hpp"

#include <QBuffer>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QTimer>
#include <QTransform>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(NYX_HAS_QT_MULTIMEDIA)

namespace {

/** Center-crop to WxH keeping aspect (no stretch). */
QImage coverCrop(const QImage& src, int w, int h) {
  if (src.isNull() || w <= 0 || h <= 0) return {};
  QImage s = src.convertToFormat(QImage::Format_RGB32)
                 .scaled(w, h, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
  const int x = std::max(0, (s.width() - w) / 2);
  const int y = std::max(0, (s.height() - h) / 2);
  return s.copy(x, y, w, h);
}

QImage applyMetaOrientation(QImage img, const QVideoFrame& frame) {
  if (img.isNull()) return img;
  switch (frame.rotationAngle()) {
    case QVideoFrame::Rotation90:
      img = img.transformed(QTransform().rotate(90), Qt::SmoothTransformation);
      break;
    case QVideoFrame::Rotation180:
      img = img.transformed(QTransform().rotate(180), Qt::SmoothTransformation);
      break;
    case QVideoFrame::Rotation270:
      img = img.transformed(QTransform().rotate(270), Qt::SmoothTransformation);
      break;
    default:
      break;
  }
  if (frame.mirrored()) img = img.mirrored(true, false);
  return img;
}

/**
 * Many Android cameras deliver landscape buffers with rotationAngle=0.
 * Rotate so a portrait-held phone shows an upright person.
 */
QImage uprightForDevice(QImage img, bool front_camera) {
  if (img.isNull()) return img;
#if defined(Q_OS_ANDROID)
  if (img.width() > img.height()) {
    // Typical sensor: back → 90° CW, front → 270° CW for portrait upright.
    const int deg = front_camera ? 270 : 90;
    img = img.transformed(QTransform().rotate(deg), Qt::SmoothTransformation);
  }
#else
  Q_UNUSED(front_camera);
#endif
  return img;
}

QCameraFormat bestFormat(const QCameraDevice& device) {
  QCameraFormat best;
  int best_score = -1;
  const int target_w = nyx::kCallVideoWidth;
  const int target_h = nyx::kCallVideoHeight;
  for (const QCameraFormat& f : device.videoFormats()) {
    if (f.resolution().width() < 160 || f.resolution().height() < 120) continue;
    const int dw = std::abs(f.resolution().width() - target_w * 2);
    const int dh = std::abs(f.resolution().height() - target_h * 2);
    const int fps = static_cast<int>(f.maxFrameRate());
    const int score = 100000 - dw - dh + std::min(fps, 30) * 10;
    if (score > best_score) {
      best_score = score;
      best = f;
    }
  }
  return best;
}

QString cameraLabel(const QCameraDevice& d) {
  QString name = d.description();
  if (name.isEmpty()) name = QString::fromUtf8(d.id());
  switch (d.position()) {
    case QCameraDevice::FrontFace:
      return QStringLiteral("%1 (передняя)").arg(name);
    case QCameraDevice::BackFace:
      return QStringLiteral("%1 (задняя)").arg(name);
    default:
      return name;
  }
}

}  // namespace

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {
  encode_timer_ = new QTimer(this);
  encode_timer_->setInterval(1000 / std::max(1, nyx::kCallVideoFps));
  connect(encode_timer_, &QTimer::timeout, this, &CallVideoIo::onEncodeTick);
}

CallVideoIo::~CallVideoIo() { stop(); }

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
  if (cams.isEmpty()) return {};
  if (!preferred_camera_id_.isEmpty()) {
    for (const auto& c : cams) {
      if (QString::fromUtf8(c.id()) == preferred_camera_id_) return c;
    }
  }
  for (const auto& c : cams) {
    if (c.position() == QCameraDevice::FrontFace) return c;
  }
  return cams.front();
}

bool CallVideoIo::openCamera(const QCameraDevice& device) {
  if (device.isNull()) return false;
  const QString new_id = QString::fromUtf8(device.id());
  if (capturing_ && camera_ && new_id == camera_id_) return true;

  capturing_ = false;
  if (session_) {
    session_->setCamera(nullptr);
    session_->setVideoSink(nullptr);
  }
  if (camera_) {
    camera_->stop();
    camera_.reset();
  }

  camera_ = std::make_unique<QCamera>(device);
  const QCameraFormat fmt = bestFormat(device);
  if (!fmt.isNull()) camera_->setCameraFormat(fmt);

  if (!session_) session_ = std::make_unique<QMediaCaptureSession>();
  if (!sink_) {
    sink_ = new QVideoSink(this);
    connect(sink_, &QVideoSink::videoFrameChanged, this, &CallVideoIo::onVideoFrame,
            Qt::UniqueConnection);
  }
  session_->setVideoSink(sink_);
  session_->setCamera(camera_.get());
  camera_id_ = new_id;
  preferred_camera_id_ = camera_id_;
  front_camera_ = (device.position() == QCameraDevice::FrontFace);
  const auto cams = QMediaDevices::videoInputs();
  camera_index_ = 0;
  for (int i = 0; i < cams.size(); ++i) {
    if (QString::fromUtf8(cams.at(i).id()) == camera_id_) {
      camera_index_ = i;
      break;
    }
  }
  connect(camera_.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString&) {
    capturing_ = false;
    if (session_) {
      session_->setCamera(nullptr);
      session_->setVideoSink(nullptr);
    }
    if (camera_) {
      camera_->stop();
      camera_.reset();
    }
    emit cameraChanged();
  });
  camera_->start();
  capturing_ = true;
  emit cameraChanged();
  return true;
}

void CallVideoIo::setPreferredCameraId(const QString& id) {
  preferred_camera_id_ = id.trimmed();
  if (running_ && capturing_ && !preferred_camera_id_.isEmpty()) {
    const QCameraDevice d = resolveCameraDevice();
    if (!d.isNull() && QString::fromUtf8(d.id()) != camera_id_) openCamera(d);
  }
  emit cameraChanged();
}

bool CallVideoIo::start() {
  if (running_) return true;
  running_ = true;

  peers_.clear();
  focused_peer_.clear();
  remote_ = QImage();
  local_ = QImage();
  pending_ = QImage();
  frame_id_ = 0;
  capturing_ = false;

  const QCameraDevice device = resolveCameraDevice();
  if (!device.isNull()) {
    if (!openCamera(device)) capturing_ = false;
  }
  if (encode_timer_) encode_timer_->start();
  return true;
}

void CallVideoIo::stop() {
  running_ = false;
  capturing_ = false;
  if (encode_timer_) encode_timer_->stop();
  if (session_) {
    session_->setCamera(nullptr);
    session_->setVideoSink(nullptr);
  }
  if (camera_) {
    camera_->stop();
    camera_.reset();
  }
  session_.reset();
  if (sink_) {
    disconnect(sink_, nullptr, this, nullptr);
    sink_->deleteLater();
    sink_ = nullptr;
  }
  pending_ = QImage();
  local_ = QImage();
  remote_ = QImage();
  focused_peer_.clear();
  peers_.clear();
  camera_id_.clear();
  camera_index_ = 0;
  front_camera_ = false;
}

bool CallVideoIo::canSwitchCamera() const {
  if (!capturing_) return false;
  const auto cams = QMediaDevices::videoInputs();
  if (cams.size() > 1) return true;
  bool has_front = false, has_back = false;
  for (const auto& c : cams) {
    if (c.position() == QCameraDevice::FrontFace) has_front = true;
    if (c.position() == QCameraDevice::BackFace) has_back = true;
  }
  return has_front && has_back;
}

bool CallVideoIo::switchCamera() {
  if (!running_ || !capturing_) return false;
  const auto cams = QMediaDevices::videoInputs();
  if (cams.isEmpty()) return false;

  const QCameraDevice::Position want =
      front_camera_ ? QCameraDevice::BackFace : QCameraDevice::FrontFace;
  for (int i = 0; i < cams.size(); ++i) {
    if (cams.at(i).position() == want) {
      camera_index_ = i;
      return openCamera(cams.at(i));
    }
  }
  if (cams.size() < 2) return false;
  camera_index_ = (camera_index_ + 1) % cams.size();
  return openCamera(cams.at(camera_index_));
}

void CallVideoIo::setFocusedPeerId(const QString& peerId) {
  focused_peer_ = peerId;
  remote_ = QImage();
  if (peerId.isEmpty()) return;
  const auto it = peers_.find(peerId.toStdString());
  if (it != peers_.end()) remote_ = it->second.frame;
}

QStringList CallVideoIo::videoPeerIds() const {
  QStringList out;
  out.reserve(static_cast<int>(peers_.size()));
  for (const auto& [id, _] : peers_) out.append(QString::fromStdString(id));
  out.sort();
  return out;
}

CallVideoIo::PeerDecoder& CallVideoIo::peerDecoder(const QString& peerId) {
  const std::string key = peerId.toStdString();
  auto it = peers_.find(key);
  if (it != peers_.end()) return it->second;
  PeerDecoder& slot = peers_[key];
  slot.reasm = std::make_unique<nyx::CallVideoReassembler>();
  emit videoPeersChanged();
  if (focused_peer_.isEmpty()) focused_peer_ = peerId;
  return slot;
}

void CallVideoIo::onVideoFrame() {
  if (!running_ || !capturing_ || !sink_) return;
  if (!pending_.isNull()) return;
  const QVideoFrame frame = sink_->videoFrame();
  if (!frame.isValid()) return;

  QImage img = uprightForDevice(applyMetaOrientation(frame.toImage(), frame), front_camera_);
  if (img.isNull()) return;

  pending_ = coverCrop(img, nyx::kCallVideoWidth, nyx::kCallVideoHeight);
  if (pending_.isNull()) return;

  local_ = front_camera_ ? pending_.mirrored(true, false) : pending_;
  emit localFrameChanged();
}

void CallVideoIo::onEncodeTick() {
  if (!running_ || !capturing_ || !send_fn_ || pending_.isNull()) return;

  QByteArray jpeg;
  QBuffer buf(&jpeg);
  if (!buf.open(QIODevice::WriteOnly)) return;
  if (!pending_.save(&buf, "JPG", 58)) return;
  buf.close();
  pending_ = QImage();
  if (jpeg.isEmpty()) return;

  nyx::ByteBuffer payload(jpeg.begin(), jpeg.end());
  const uint16_t fid = frame_id_++;
  auto frags = nyx::fragment_av1_frame(fid, true, payload, nyx::kMaxCallMediaPayload);
  for (auto& frag : frags) {
    QByteArray bytes(reinterpret_cast<const char*>(frag.data()),
                     static_cast<int>(frag.size()));
    send_fn_(bytes);
  }
}

void CallVideoIo::onRemoteVideo(const QString& peerId, const QByteArray& frag_payload) {
  if (frag_payload.isEmpty()) return;
  const QString pid = peerId.isEmpty() ? QStringLiteral("direct") : peerId;
  auto& peer = peerDecoder(pid);
  if (!peer.reasm) return;

  nyx::ByteBuffer buf(frag_payload.begin(), frag_payload.end());
  auto full = peer.reasm->push(buf);
  if (!full) return;

  QImage img;
  if (!img.loadFromData(full->data.data(), static_cast<int>(full->data.size()), "JPG")) return;
  peer.frame = img.convertToFormat(QImage::Format_RGB32);
  if (peer.frame.isNull()) return;

  if (focused_peer_.isEmpty() || focused_peer_ == pid) {
    focused_peer_ = pid;
    remote_ = peer.frame;
  }
  emit remoteFrameChanged(pid);
}

#else

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {}
CallVideoIo::~CallVideoIo() = default;
bool CallVideoIo::start() { return false; }
void CallVideoIo::stop() {}
void CallVideoIo::onVideoFrame() {}
void CallVideoIo::onEncodeTick() {}
void CallVideoIo::onRemoteVideo(const QString&, const QByteArray&) {}
bool CallVideoIo::canSwitchCamera() const { return false; }
bool CallVideoIo::switchCamera() { return false; }
void CallVideoIo::setFocusedPeerId(const QString&) {}
QStringList CallVideoIo::videoPeerIds() const { return {}; }
void CallVideoIo::setPreferredCameraId(const QString&) {}
QVariantList CallVideoIo::listCameraDevices() { return {}; }
CallVideoIo::PeerDecoder& CallVideoIo::peerDecoder(const QString&) {
  static PeerDecoder dummy;
  return dummy;
}
bool CallVideoIo::openCamera(const QCameraDevice&) { return false; }
QCameraDevice CallVideoIo::resolveCameraDevice() const { return {}; }

#endif
