#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>

class ChatMediaRecorderWorker;

class ChatMediaRecorder : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString state READ state NOTIFY stateChanged)
  Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
  Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY elapsedChanged)
  Q_PROPERTY(QString error READ error NOTIFY stateChanged)

 public:
  explicit ChatMediaRecorder(QObject* parent = nullptr);
  ~ChatMediaRecorder() override;

  QString state() const { return state_; }
  bool recording() const {
    return state_ == QLatin1String("starting") ||
           state_ == QLatin1String("recording") ||
           state_ == QLatin1String("stopping");
  }
  qint64 elapsedMs() const { return elapsed_ms_; }
  QString error() const { return error_; }

  Q_INVOKABLE void startVoice(const QString& outputPath);
  Q_INVOKABLE void stopVoice(bool send);
  Q_INVOKABLE void cancel();

 signals:
  void stateChanged();
  void elapsedChanged();
  void ready(const QString& path, const QString& mime,
             const QString& displayName, const QString& mediaKind);
  void startWorker(const QString& outputPath);
  void stopWorker();

 private slots:
  void onWorkerStarted();
  void onWorkerStopped(const QString& actualPath);
  void onWorkerFailed(const QString& message);

 private:
  static void permissionResult(bool micOk, bool cameraOk, void* ctx);
  void beginAfterPermission(bool granted);
  void setState(const QString& state, const QString& error = {});
  void reset(bool removeFile);

  QThread worker_thread_;
  ChatMediaRecorderWorker* worker_ = nullptr;
  QTimer elapsed_timer_;
  QTimer stop_timeout_;
  QString state_ = QStringLiteral("idle");
  QString error_;
  QString output_path_;
  bool send_on_stop_ = false;
  qint64 started_at_ms_ = 0;
  qint64 elapsed_ms_ = 0;
};
