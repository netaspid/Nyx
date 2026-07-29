#pragma once

#include "call_audio_io.hpp"
#include "call_frame_provider.hpp"
#include "call_video_io.hpp"

#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>

class NodeController;

class CallUi : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString callState READ callState NOTIFY callChanged)
  Q_PROPERTY(QString callTitle READ callTitle NOTIFY callChanged)
  Q_PROPERTY(bool callVideo READ callVideo NOTIFY callChanged)
  Q_PROPERTY(bool canStartCall READ canStartCall NOTIFY canStartCallChanged)
  Q_PROPERTY(bool callIsFieldRoom READ callIsFieldRoom NOTIFY callChanged)
  Q_PROPERTY(bool callMicMuted READ callMicMuted WRITE setCallMicMuted NOTIFY callChanged)
  Q_PROPERTY(bool callCameraOn READ callCameraOn WRITE setCallCameraOn NOTIFY callChanged)
  Q_PROPERTY(
      bool callSpeakerphone READ callSpeakerphone WRITE setCallSpeakerphone NOTIFY callChanged)
  Q_PROPERTY(float audioTestLevel READ audioTestLevel NOTIFY audioTestLevelChanged)
  Q_PROPERTY(bool audioTestActive READ audioTestActive NOTIFY audioTestChanged)
  Q_PROPERTY(QUrl callRemoteFrameUrl READ callRemoteFrameUrl NOTIFY callRemoteFrameChanged)
  Q_PROPERTY(QUrl callLocalFrameUrl READ callLocalFrameUrl NOTIFY callLocalFrameChanged)
  Q_PROPERTY(bool callCanSwitchCamera READ callCanSwitchCamera NOTIFY callChanged)
  Q_PROPERTY(QVariantList callVideoPeers READ callVideoPeers NOTIFY callVideoPeersChanged)
  Q_PROPERTY(QVariantList callRosterPeers READ callRosterPeers NOTIFY callVideoPeersChanged)
  Q_PROPERTY(QVariantList cameraDeviceList READ cameraDeviceList NOTIFY mediaDevicesChanged)
  Q_PROPERTY(QVariantList audioInputDeviceList READ audioInputDeviceList NOTIFY mediaDevicesChanged)
  Q_PROPERTY(
      QVariantList audioOutputDeviceList READ audioOutputDeviceList NOTIFY mediaDevicesChanged)
  Q_PROPERTY(QString selectedCameraId READ selectedCameraId WRITE setSelectedCameraId NOTIFY
                 mediaDevicesChanged)
  Q_PROPERTY(QString selectedAudioInputId READ selectedAudioInputId WRITE setSelectedAudioInputId
                 NOTIFY mediaDevicesChanged)
  Q_PROPERTY(QString selectedAudioOutputId READ selectedAudioOutputId WRITE setSelectedAudioOutputId
                 NOTIFY mediaDevicesChanged)

public:
  explicit CallUi(QObject* parent = nullptr) : QObject(parent) {}

  void setHost(NodeController* host) { host_ = host; }

  QString callState() const;
  QString callTitle() const;
  bool callVideo() const;
  bool canStartCall() const;
  bool callIsFieldRoom() const;
  bool callMicMuted() const;
  void setCallMicMuted(bool muted);
  bool callCameraOn() const;
  void setCallCameraOn(bool on);
  bool callSpeakerphone() const;
  void setCallSpeakerphone(bool on);
  float audioTestLevel() const;
  bool audioTestActive() const;
  QUrl callRemoteFrameUrl() const { return call_remote_frame_url_; }
  QUrl callLocalFrameUrl() const { return call_local_frame_url_; }
  bool callCanSwitchCamera() const;
  QVariantList callVideoPeers() const;
  QVariantList callRosterPeers() const;
  QVariantList cameraDeviceList() const;
  QVariantList audioInputDeviceList() const;
  QVariantList audioOutputDeviceList() const;
  QString selectedCameraId() const;
  QString selectedAudioInputId() const;
  QString selectedAudioOutputId() const;
  void setSelectedCameraId(const QString& id);
  void setSelectedAudioInputId(const QString& id);
  void setSelectedAudioOutputId(const QString& id);

  Q_INVOKABLE void startCall(bool video = false);
  Q_INVOKABLE void acceptCall();
  Q_INVOKABLE void rejectCall();
  Q_INVOKABLE void hangupCall();
  Q_INVOKABLE void switchCallCamera();
  Q_INVOKABLE void setCallFocusedPeer(const QString& peerIdHex);
  Q_INVOKABLE void toggleCallMicMuted();
  Q_INVOKABLE void toggleCallCamera();
  Q_INVOKABLE void toggleCallSpeakerphone();
  Q_INVOKABLE void refreshMediaDevices();
  Q_INVOKABLE void startMicTest();
  Q_INVOKABLE void stopAudioTest();
  Q_INVOKABLE void playSpeakerTest();

  void notifyCallChanged() { emit callChanged(); }
  void notifyCallRemoteFrameChanged() { emit callRemoteFrameChanged(); }
  void notifyCallLocalFrameChanged() { emit callLocalFrameChanged(); }
  void notifyCallVideoPeersChanged() { emit callVideoPeersChanged(); }
  void notifyMediaDevicesChanged() { emit mediaDevicesChanged(); }
  void notifyAudioTestLevelChanged() { emit audioTestLevelChanged(); }
  void notifyAudioTestChanged() { emit audioTestChanged(); }
  void notifyCanStartCallChanged() { emit canStartCallChanged(); }

signals:
  void callChanged();
  void canStartCallChanged();
  void callRemoteFrameChanged();
  void callLocalFrameChanged();
  void callVideoPeersChanged();
  void mediaDevicesChanged();
  void audioTestLevelChanged();
  void audioTestChanged();

public:
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

private:
  NodeController* host_ = nullptr;
};
