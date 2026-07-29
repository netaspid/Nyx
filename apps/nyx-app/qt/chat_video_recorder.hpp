#pragma once

#include <QObject>
#include <QTimer>

class CallFrameProvider;

class ChatVideoRecorder : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString state READ state NOTIFY stateChanged)
  Q_PROPERTY(QString error READ error NOTIFY stateChanged)
  Q_PROPERTY(QString outputPath READ outputPath NOTIFY stateChanged)
  Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY elapsedChanged)
  Q_PROPERTY(int frameEpoch READ frameEpoch NOTIFY frameChanged)
  Q_PROPERTY(bool canSwitchCamera READ canSwitchCamera NOTIFY stateChanged)

 public:
  explicit ChatVideoRecorder(QObject* parent = nullptr);
  ~ChatVideoRecorder() override;

  QString state() const { return state_; }
  QString error() const { return error_; }
  QString outputPath() const { return output_path_; }
  qint64 elapsedMs() const { return elapsed_ms_; }
  int frameEpoch() const { return frame_epoch_; }
  bool canSwitchCamera() const { return can_switch_camera_; }

  void setFrameProvider(CallFrameProvider* provider) { frame_provider_ = provider; }

  Q_INVOKABLE void openPreview();
  Q_INVOKABLE void startRecording(const QString& outputPath);
  Q_INVOKABLE void stopRecording();
  Q_INVOKABLE void discardRecording();
  Q_INVOKABLE void finish(bool send);
  Q_INVOKABLE void close();
  Q_INVOKABLE void switchCamera();

 signals:
  void stateChanged();
  void elapsedChanged();
  void frameChanged();
  void ready(const QString& path, const QString& mime,
             const QString& displayName, const QString& mediaKind);

 private:
  static void permissionResult(bool micOk, bool cameraOk, void* ctx);
  void beginAfterPermission(bool granted);
  void onJpeg(const QByteArray& jpeg, bool front);
  void onCameraError(const QString& message);
  void onCameraStarted(bool front, const QString& cameraId);
  void onRecordingStarted(const QString& path);
  void onRecordingStopped(const QString& path, bool success);
  void onRecordingError(const QString& message);
  void setState(const QString& state, const QString& error = {});
  void reset(bool removeFile);

  CallFrameProvider* frame_provider_ = nullptr;
  QTimer elapsed_timer_;
  QTimer operation_timeout_;
  QString state_ = QStringLiteral("idle");
  QString error_;
  QString output_path_;
  bool front_camera_ = true;
  bool can_switch_camera_ = false;
  bool close_on_stop_ = false;
  qint64 started_at_ms_ = 0;
  qint64 elapsed_ms_ = 0;
  int frame_epoch_ = 0;
};
