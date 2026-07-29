#pragma once

#include "call_audio_io.hpp"
#include "call_frame_provider.hpp"
#include "call_video_io.hpp"

#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>

class CallUi : public QObject {
  Q_OBJECT
public:
  explicit CallUi(QObject* parent = nullptr) : QObject(parent) {}

  QThread call_audio_thread_;
  CallAudioIo call_audio_;
  QThread call_video_thread_;
  CallVideoIo call_video_;
  CallFrameProvider* call_frames_ = nullptr;
  bool call_video_slots_wired_ = false;
  QUrl call_remote_frame_url_;
  QUrl call_local_frame_url_;
  int call_frame_epoch_ = 0;
  bool call_speakerphone_ = true;
  QString last_call_notify_key_;
  bool answering_call_ = false;
  bool resume_call_camera_ = false;
  QString suspended_call_id_;
  QString manual_call_focus_;
  qint64 manual_call_focus_until_ms_ = 0;
  qint64 call_media_started_ms_ = 0;
};
