#include "chat_video_recorder.hpp"

#include "android_platform.hpp"
#include "call_frame_provider.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QTransform>

namespace {

QImage uprightPreview(QImage image, bool front) {
  if (image.isNull())
    return image;
#if defined(Q_OS_ANDROID)
  if (image.width() > image.height()) {
    image = image.transformed(QTransform().rotate(front ? 270 : 90), Qt::FastTransformation);
  }
  if (front)
    image = image.mirrored(true, false);
#else
  Q_UNUSED(front);
#endif
  return image;
}

}

ChatVideoRecorder::ChatVideoRecorder(QObject* parent) : QObject(parent) {
  elapsed_timer_.setInterval(100);
  connect(&elapsed_timer_, &QTimer::timeout, this, [this]() {
    if (started_at_ms_ <= 0)
      return;
    elapsed_ms_ = QDateTime::currentMSecsSinceEpoch() - started_at_ms_;
    emit elapsedChanged();
    if (elapsed_ms_ >= 60000 && state_ == QLatin1String("recording"))
      stopRecording();
  });

  operation_timeout_.setSingleShot(true);
  connect(&operation_timeout_, &QTimer::timeout, this, [this]() {
    onRecordingError(state_ == QLatin1String("starting")
                         ? QStringLiteral("Camera start timed out")
                         : QStringLiteral("Video recorder timed out"));
  });
}

ChatVideoRecorder::~ChatVideoRecorder() {
  nyx_android::set_native_camera_callbacks(nullptr, nullptr, nullptr, nullptr);
  nyx_android::set_native_recording_callbacks(nullptr, nullptr, nullptr, nullptr);
  nyx_android::native_camera_stop();
}

void ChatVideoRecorder::openPreview() {
#if defined(Q_OS_ANDROID)
  if (state_ != QLatin1String("idle") && state_ != QLatin1String("failed"))
    return;
  reset(true);
  setState(QStringLiteral("starting"));
  operation_timeout_.start(10000);
  nyx_android::request_call_permissions(true, &ChatVideoRecorder::permissionResult, this);
#else
  setState(QStringLiteral("failed"), QStringLiteral("Native camera is available only on Android"));
#endif
}

void ChatVideoRecorder::startRecording(const QString& outputPath) {
#if defined(Q_OS_ANDROID)
  if (state_ != QLatin1String("idle") || outputPath.trimmed().isEmpty())
    return;
  output_path_ = outputPath.trimmed();
  QFile::remove(output_path_);
  elapsed_ms_ = 0;
  started_at_ms_ = 0;
  setState(QStringLiteral("starting-recording"));
  operation_timeout_.start(12000);
  nyx_android::native_camera_start_recording(output_path_);
#else
  Q_UNUSED(outputPath);
#endif
}

void ChatVideoRecorder::stopRecording() {
#if defined(Q_OS_ANDROID)
  if (state_ != QLatin1String("recording"))
    return;
  elapsed_timer_.stop();
  setState(QStringLiteral("stopping"));
  operation_timeout_.start(15000);
  nyx_android::native_camera_stop_recording();
#endif
}

void ChatVideoRecorder::discardRecording() {
  if (state_ != QLatin1String("preview"))
    return;
  QFile::remove(output_path_);
  output_path_.clear();
  elapsed_ms_ = 0;
  started_at_ms_ = 0;
  setState(QStringLiteral("idle"));
  emit elapsedChanged();
}

void ChatVideoRecorder::finish(bool send) {
  if (state_ != QLatin1String("preview"))
    return;
  const QString path = output_path_;
  if (send && QFileInfo(path).isFile() && QFileInfo(path).size() > 0) {
    output_path_.clear();
    emit ready(path,
               QStringLiteral("video/mp4"),
               QStringLiteral("circle-message.mp4"),
               QStringLiteral("circle"));
  } else {
    QFile::remove(path);
  }
  close();
}

void ChatVideoRecorder::close() {
#if defined(Q_OS_ANDROID)
  if (state_ == QLatin1String("recording")) {
    close_on_stop_ = true;
    stopRecording();
    return;
  }
  if (state_ == QLatin1String("stopping")) {
    close_on_stop_ = true;
    return;
  }
  operation_timeout_.stop();
  elapsed_timer_.stop();
  nyx_android::set_native_camera_callbacks(nullptr, nullptr, nullptr, nullptr);
  nyx_android::set_native_recording_callbacks(nullptr, nullptr, nullptr, nullptr);
  nyx_android::native_camera_stop();
  reset(true);
#endif
}

void ChatVideoRecorder::switchCamera() {
#if defined(Q_OS_ANDROID)
  if (state_ != QLatin1String("idle") || !can_switch_camera_)
    return;
  setState(QStringLiteral("starting"));
  operation_timeout_.start(10000);
  nyx_android::native_camera_switch_facing();
#endif
}

void ChatVideoRecorder::permissionResult(bool micOk, bool cameraOk, void* ctx) {
  auto* self = static_cast<ChatVideoRecorder*>(ctx);
  if (!self)
    return;
  QMetaObject::invokeMethod(
      self,
      [self, micOk, cameraOk]() { self->beginAfterPermission(micOk && cameraOk); },
      Qt::QueuedConnection);
}

void ChatVideoRecorder::beginAfterPermission(bool granted) {
  if (state_ != QLatin1String("starting"))
    return;
  if (!granted) {
    operation_timeout_.stop();
    setState(QStringLiteral("failed"),
             QStringLiteral("Camera or microphone permission was denied"));
    return;
  }

  nyx_android::set_native_camera_callbacks(
      [](const QByteArray& jpeg, bool front, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onJpeg(jpeg, front);
      },
      [](const QString& message, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onCameraError(message);
      },
      [](bool front, const QString& cameraId, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onCameraStarted(front, cameraId);
      },
      this);
  nyx_android::set_native_recording_callbacks(
      [](const QString& path, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onRecordingStarted(path);
      },
      [](const QString& path, bool success, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onRecordingStopped(path, success);
      },
      [](const QString& message, void* ctx) {
        static_cast<ChatVideoRecorder*>(ctx)->onRecordingError(message);
      },
      this);
  can_switch_camera_ = nyx_android::native_camera_has_front_and_back();
  nyx_android::native_camera_start(front_camera_);
  emit stateChanged();
}

void ChatVideoRecorder::onJpeg(const QByteArray& jpeg, bool front) {
  if (!frame_provider_ || jpeg.isEmpty())
    return;
  QImage image;
  if (!image.loadFromData(jpeg, "JPG"))
    return;
  image = uprightPreview(std::move(image), front);
  if (image.isNull())
    return;
  frame_provider_->setLocal(image);
  ++frame_epoch_;
  emit frameChanged();
}

void ChatVideoRecorder::onCameraError(const QString& message) {
  operation_timeout_.stop();
  elapsed_timer_.stop();
  setState(QStringLiteral("failed"), message.isEmpty() ? QStringLiteral("Camera failed") : message);
}

void ChatVideoRecorder::onCameraStarted(bool front, const QString&) {
  if (state_ != QLatin1String("starting"))
    return;
  operation_timeout_.stop();
  front_camera_ = front;
  setState(QStringLiteral("idle"));
}

void ChatVideoRecorder::onRecordingStarted(const QString& path) {
  if (state_ != QLatin1String("starting-recording"))
    return;
  operation_timeout_.stop();
  if (!path.isEmpty())
    output_path_ = path;
  started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
  elapsed_ms_ = 0;
  setState(QStringLiteral("recording"));
  elapsed_timer_.start();
}

void ChatVideoRecorder::onRecordingStopped(const QString& path, bool success) {
  operation_timeout_.stop();
  elapsed_timer_.stop();
  if (!path.isEmpty())
    output_path_ = path;
  const bool valid = success && elapsed_ms_ >= 700 && QFileInfo(output_path_).isFile() &&
                     QFileInfo(output_path_).size() > 0;
  if (!valid) {
    QFile::remove(output_path_);
    output_path_.clear();
    if (close_on_stop_) {
      close_on_stop_ = false;
      close();
    } else {
      setState(QStringLiteral("idle"));
    }
    return;
  }
  setState(QStringLiteral("preview"));
  if (close_on_stop_) {
    close_on_stop_ = false;
    finish(false);
  }
}

void ChatVideoRecorder::onRecordingError(const QString& message) {
  operation_timeout_.stop();
  elapsed_timer_.stop();
  QFile::remove(output_path_);
  output_path_.clear();
  setState(QStringLiteral("failed"),
           message.isEmpty() ? QStringLiteral("Video recording failed") : message);
}

void ChatVideoRecorder::setState(const QString& state, const QString& error) {
  if (state_ == state && error_ == error)
    return;
  state_ = state;
  error_ = error;
  emit stateChanged();
}

void ChatVideoRecorder::reset(bool removeFile) {
  if (removeFile && !output_path_.isEmpty())
    QFile::remove(output_path_);
  operation_timeout_.stop();
  elapsed_timer_.stop();
  if (frame_provider_)
    frame_provider_->clear();
  output_path_.clear();
  error_.clear();
  close_on_stop_ = false;
  started_at_ms_ = 0;
  elapsed_ms_ = 0;
  frame_epoch_ = 0;
  setState(QStringLiteral("idle"));
  emit elapsedChanged();
  emit frameChanged();
}
