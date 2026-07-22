#include "call_video_io.hpp"

#include "nyx/call_av1.hpp"
#include "nyx/call_media.hpp"

#include <QCamera>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(NYX_HAS_QT_MULTIMEDIA)

namespace {

QImage i420_to_qimage(const uint8_t* i420, int w, int h) {
  if (!i420 || w <= 0 || h <= 0) return {};
  QImage img(w, h, QImage::Format_RGB888);
  const int y_sz = w * h;
  const uint8_t* y = i420;
  const uint8_t* u = i420 + y_sz;
  const uint8_t* v = u + (w / 2) * (h / 2);
  for (int row = 0; row < h; ++row) {
    uchar* dst = img.scanLine(row);
    for (int col = 0; col < w; ++col) {
      const int Y = y[row * w + col];
      const int U = u[(row / 2) * (w / 2) + (col / 2)] - 128;
      const int V = v[(row / 2) * (w / 2) + (col / 2)] - 128;
      int r = Y + ((1436 * V) >> 10);
      int g = Y - ((352 * U + 731 * V) >> 10);
      int b = Y + ((1814 * U) >> 10);
      dst[col * 3 + 0] = static_cast<uchar>(std::clamp(r, 0, 255));
      dst[col * 3 + 1] = static_cast<uchar>(std::clamp(g, 0, 255));
      dst[col * 3 + 2] = static_cast<uchar>(std::clamp(b, 0, 255));
    }
  }
  return img;
}

std::vector<uint8_t> qimage_to_i420(const QImage& src, int w, int h) {
  QImage img = src.convertToFormat(QImage::Format_RGB888).scaled(
      w, h, Qt::IgnoreAspectRatio, Qt::FastTransformation);
  const int y_sz = w * h;
  const int uv_sz = (w / 2) * (h / 2);
  std::vector<uint8_t> out(static_cast<std::size_t>(y_sz + 2 * uv_sz));
  uint8_t* y = out.data();
  uint8_t* u = y + y_sz;
  uint8_t* v = u + uv_sz;
  for (int row = 0; row < h; ++row) {
    const uchar* s = img.constScanLine(row);
    for (int col = 0; col < w; ++col) {
      const int r = s[col * 3 + 0];
      const int g = s[col * 3 + 1];
      const int b = s[col * 3 + 2];
      y[row * w + col] = static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
      if ((row % 2) == 0 && (col % 2) == 0) {
        const int ui = (row / 2) * (w / 2) + (col / 2);
        u[ui] = static_cast<uint8_t>(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
        v[ui] = static_cast<uint8_t>(((128 * r - 107 * g - 21 * b) >> 8) + 128);
      }
    }
  }
  return out;
}

}  // namespace

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {
  encode_timer_ = new QTimer(this);
  encode_timer_->setInterval(1000 / std::max(1, nyx::kCallVideoFps));
  connect(encode_timer_, &QTimer::timeout, this, &CallVideoIo::onEncodeTick);
}

CallVideoIo::~CallVideoIo() { stop(); }

bool CallVideoIo::start() {
  if (running_) return true;
  const QCameraDevice cam = QMediaDevices::defaultVideoInput();
  if (cam.isNull()) return false;

  encoder_ = std::make_unique<nyx::Av1Encoder>();
  decoder_ = std::make_unique<nyx::Av1Decoder>();
  reasm_ = std::make_unique<nyx::CallVideoReassembler>();
  if (!encoder_->ok() || !decoder_->ok()) {
    encoder_.reset();
    decoder_.reset();
    reasm_.reset();
    return false;
  }

  camera_ = std::make_unique<QCamera>(cam);
  session_ = std::make_unique<QMediaCaptureSession>();
  sink_ = new QVideoSink(this);
  session_->setCamera(camera_.get());
  session_->setVideoSink(sink_);
  connect(sink_, &QVideoSink::videoFrameChanged, this, &CallVideoIo::onVideoFrame);

  camera_->start();
  encode_timer_->start();
  running_ = true;
  keyframe_counter_ = 0;
  return true;
}

void CallVideoIo::stop() {
  running_ = false;
  if (encode_timer_) encode_timer_->stop();
  if (camera_) {
    camera_->stop();
    camera_.reset();
  }
  session_.reset();
  sink_ = nullptr;
  pending_ = QImage();
  encoder_.reset();
  decoder_.reset();
  reasm_.reset();
}

void CallVideoIo::onVideoFrame() {
  if (!running_ || !sink_) return;
  const QVideoFrame frame = sink_->videoFrame();
  if (!frame.isValid()) return;
  QVideoFrame f(frame);
  if (!f.map(QVideoFrame::ReadOnly)) return;
  const QImage img = f.toImage();
  f.unmap();
  if (img.isNull()) return;
  pending_ = img.scaled(nyx::kCallVideoWidth, nyx::kCallVideoHeight, Qt::IgnoreAspectRatio,
                        Qt::FastTransformation);
  emit localFrameChanged(pending_);
}

void CallVideoIo::onEncodeTick() {
  if (!running_ || !send_fn_ || !encoder_ || pending_.isNull()) return;

  const bool force_kf = (keyframe_counter_++ % (nyx::kCallVideoFps * 2) == 0);
  auto i420 = qimage_to_i420(pending_, nyx::kCallVideoWidth, nyx::kCallVideoHeight);
  auto encoded = encoder_->encode_i420(i420.data(), nyx::kCallVideoWidth, nyx::kCallVideoHeight,
                                       force_kf);
  if (!encoded) return;

  const uint16_t fid = frame_id_++;
  auto frags = nyx::fragment_av1_frame(fid, force_kf, *encoded, nyx::kMaxCallMediaPayload);
  for (auto& frag : frags) {
    QByteArray bytes(reinterpret_cast<const char*>(frag.data()),
                     static_cast<int>(frag.size()));
    send_fn_(bytes);
  }
}

void CallVideoIo::onRemoteVideo(const QByteArray& frag_payload) {
  if (!decoder_ || !reasm_ || frag_payload.isEmpty()) return;
  nyx::ByteBuffer buf(frag_payload.begin(), frag_payload.end());
  auto full = reasm_->push(buf);
  if (!full) return;
  auto frame = decoder_->decode(full->data(), full->size());
  if (!frame) return;
  remote_ = i420_to_qimage(frame->i420.data(), frame->width, frame->height);
  if (!remote_.isNull()) emit remoteFrameChanged();
}

#else

CallVideoIo::CallVideoIo(QObject* parent) : QObject(parent) {}
CallVideoIo::~CallVideoIo() = default;
bool CallVideoIo::start() { return false; }
void CallVideoIo::stop() {}
void CallVideoIo::onVideoFrame() {}
void CallVideoIo::onEncodeTick() {}
void CallVideoIo::onRemoteVideo(const QByteArray&) {}

#endif
