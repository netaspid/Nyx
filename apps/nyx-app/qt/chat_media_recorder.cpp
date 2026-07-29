#include "chat_media_recorder.hpp"

#include "android_platform.hpp"

#include <QAudioInput>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QMetaObject>
#include <QUrl>

class ChatMediaRecorderWorker : public QObject {
  Q_OBJECT

 public slots:
  void start(const QString& outputPath) {
    dispose();
    audio_ = new QAudioInput(this);
    recorder_ = new QMediaRecorder(this);
    session_ = new QMediaCaptureSession(this);
    session_->setAudioInput(audio_);
    session_->setRecorder(recorder_);

    QMediaFormat format;
    format.setFileFormat(QMediaFormat::MPEG4);
    format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    recorder_->setMediaFormat(format);
    recorder_->setQuality(QMediaRecorder::HighQuality);
    recorder_->setOutputLocation(QUrl::fromLocalFile(outputPath));

    connect(recorder_, &QMediaRecorder::recorderStateChanged, this,
            [this](QMediaRecorder::RecorderState state) {
              if (state == QMediaRecorder::RecordingState) {
                emit started();
              } else if (state == QMediaRecorder::StoppedState &&
                         stop_requested_) {
                const QString actual = recorder_->actualLocation().isLocalFile()
                                           ? recorder_->actualLocation().toLocalFile()
                                           : output_path_;
                stop_requested_ = false;
                emit stopped(actual);
              }
            });
    connect(recorder_, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString& message) {
              emit failed(message.isEmpty() ? QStringLiteral("Audio recorder failed")
                                            : message);
            });

    output_path_ = outputPath;
    stop_requested_ = false;
    recorder_->record();
  }

  void stop() {
    if (!recorder_) {
      emit failed(QStringLiteral("Audio recorder is not running"));
      return;
    }
    stop_requested_ = true;
    recorder_->stop();
  }

 signals:
  void started();
  void stopped(const QString& actualPath);
  void failed(const QString& message);

 private:
  void dispose() {
    if (recorder_ && recorder_->recorderState() != QMediaRecorder::StoppedState)
      recorder_->stop();
    delete session_;
    session_ = nullptr;
    recorder_ = nullptr;
    audio_ = nullptr;
    output_path_.clear();
    stop_requested_ = false;
  }

  QAudioInput* audio_ = nullptr;
  QMediaRecorder* recorder_ = nullptr;
  QMediaCaptureSession* session_ = nullptr;
  QString output_path_;
  bool stop_requested_ = false;
};

ChatMediaRecorder::ChatMediaRecorder(QObject* parent) : QObject(parent) {
  worker_ = new ChatMediaRecorderWorker();
  worker_->moveToThread(&worker_thread_);
  connect(&worker_thread_, &QThread::finished, worker_, &QObject::deleteLater);
  connect(this, &ChatMediaRecorder::startWorker, worker_,
          &ChatMediaRecorderWorker::start, Qt::QueuedConnection);
  connect(this, &ChatMediaRecorder::stopWorker, worker_,
          &ChatMediaRecorderWorker::stop, Qt::QueuedConnection);
  connect(worker_, &ChatMediaRecorderWorker::started, this,
          &ChatMediaRecorder::onWorkerStarted, Qt::QueuedConnection);
  connect(worker_, &ChatMediaRecorderWorker::stopped, this,
          &ChatMediaRecorder::onWorkerStopped, Qt::QueuedConnection);
  connect(worker_, &ChatMediaRecorderWorker::failed, this,
          &ChatMediaRecorder::onWorkerFailed, Qt::QueuedConnection);

  elapsed_timer_.setInterval(100);
  connect(&elapsed_timer_, &QTimer::timeout, this, [this]() {
    if (started_at_ms_ <= 0) return;
    elapsed_ms_ = QDateTime::currentMSecsSinceEpoch() - started_at_ms_;
    emit elapsedChanged();
    if (elapsed_ms_ >= 5 * 60 * 1000) stopVoice(true);
  });

  stop_timeout_.setSingleShot(true);
  stop_timeout_.setInterval(8000);
  connect(&stop_timeout_, &QTimer::timeout, this, [this]() {
    onWorkerFailed(state_ == QLatin1String("starting")
                       ? QStringLiteral("Recorder start timed out")
                       : QStringLiteral("Recorder finalization timed out"));
  });
  worker_thread_.setObjectName(QStringLiteral("nyx-chat-audio"));
  worker_thread_.start();
}

ChatMediaRecorder::~ChatMediaRecorder() {
  emit stopWorker();
  worker_thread_.quit();
  worker_thread_.wait(3000);
}

void ChatMediaRecorder::startVoice(const QString& outputPath) {
  if (recording() || outputPath.trimmed().isEmpty()) return;
  output_path_ = outputPath;
  send_on_stop_ = false;
  elapsed_ms_ = 0;
  error_.clear();
  setState(QStringLiteral("starting"));
  nyx_android::request_call_permissions(false, &ChatMediaRecorder::permissionResult,
                                        this);
}

void ChatMediaRecorder::stopVoice(bool send) {
  if (!recording() || state_ == QLatin1String("stopping")) return;
  send_on_stop_ = send;
  setState(QStringLiteral("stopping"));
  elapsed_timer_.stop();
  stop_timeout_.start();
  emit stopWorker();
}

void ChatMediaRecorder::cancel() {
  if (recording()) {
    stopVoice(false);
  } else {
    reset(true);
  }
}

void ChatMediaRecorder::permissionResult(bool micOk, bool, void* ctx) {
  auto* self = static_cast<ChatMediaRecorder*>(ctx);
  if (!self) return;
  QMetaObject::invokeMethod(
      self, [self, micOk]() { self->beginAfterPermission(micOk); },
      Qt::QueuedConnection);
}

void ChatMediaRecorder::beginAfterPermission(bool granted) {
  if (state_ != QLatin1String("starting")) return;
  if (!granted) {
    setState(QStringLiteral("failed"),
             QStringLiteral("Microphone permission was denied"));
    return;
  }
  QFile::remove(output_path_);
  stop_timeout_.start();
  emit startWorker(output_path_);
}

void ChatMediaRecorder::onWorkerStarted() {
  if (state_ != QLatin1String("starting")) return;
  stop_timeout_.stop();
  started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
  elapsed_ms_ = 0;
  setState(QStringLiteral("recording"));
  elapsed_timer_.start();
}

void ChatMediaRecorder::onWorkerStopped(const QString& actualPath) {
  stop_timeout_.stop();
  elapsed_timer_.stop();
  const QString path = actualPath.isEmpty() ? output_path_ : actualPath;
  const bool valid = QFileInfo(path).isFile() && QFileInfo(path).size() > 0 &&
                     elapsed_ms_ >= 400;
  if (!send_on_stop_ || !valid) {
    if (QFileInfo::exists(path)) QFile::remove(path);
    reset(false);
    return;
  }
  setState(QStringLiteral("ready"));
  emit ready(path, QStringLiteral("audio/mp4"),
             QStringLiteral("voice-message.m4a"),
             QStringLiteral("voice"));
  reset(false);
}

void ChatMediaRecorder::onWorkerFailed(const QString& message) {
  stop_timeout_.stop();
  elapsed_timer_.stop();
  if (!output_path_.isEmpty()) QFile::remove(output_path_);
  setState(QStringLiteral("failed"), message);
  QTimer::singleShot(1500, this, [this]() {
    if (state_ == QLatin1String("failed")) reset(false);
  });
}

void ChatMediaRecorder::setState(const QString& state, const QString& error) {
  if (state_ == state && error_ == error) return;
  state_ = state;
  error_ = error;
  emit stateChanged();
}

void ChatMediaRecorder::reset(bool removeFile) {
  if (removeFile && !output_path_.isEmpty()) QFile::remove(output_path_);
  stop_timeout_.stop();
  elapsed_timer_.stop();
  output_path_.clear();
  send_on_stop_ = false;
  started_at_ms_ = 0;
  elapsed_ms_ = 0;
  error_.clear();
  setState(QStringLiteral("idle"));
  emit elapsedChanged();
}

#include "chat_media_recorder.moc"
