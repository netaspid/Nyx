#include "node_controller.hpp"

#include "android_platform.hpp"

#include "nyx/util.hpp"

#include <QDateTime>
#include <QImage>
#include <QMetaObject>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

QString NodeController::callState() const {
  switch (service_.call_state()) {
  case nyx::CallState::Outgoing:
    return QStringLiteral("outgoing");
  case nyx::CallState::Incoming:
    return QStringLiteral("incoming");
  case nyx::CallState::Ringing:
    return QStringLiteral("ringing");
  case nyx::CallState::Active:
    return QStringLiteral("active");
  case nyx::CallState::Ended:
    return QStringLiteral("ended");
  case nyx::CallState::Idle:
  default:
    return QStringLiteral("idle");
  }
}

QString NodeController::callTitle() const {
  const QString t = QString::fromStdString(service_.call_title());
  return t.isEmpty() ? peer_title_ : t;
}

bool NodeController::callVideo() const {
  return service_.call_mode() == nyx::CallMode::AudioVideo;
}

bool NodeController::canStartCall() const {
  return service_.can_start_call(active_chat_key_.toStdString());
}

bool NodeController::callIsFieldRoom() const {
  return service_.call_is_field_room();
}

void NodeController::startCall(bool video) {
  const std::string key = active_chat_key_.toStdString();
  if (!service_.is_session_up(key)) {

    if (service_.ensure_session(key)) {
      showToast(QStringLiteral("Нет связи — переподключаюсь. Позвоните ещё раз через пару секунд."),
                false);
    } else {
      showToast(QStringLiteral("Нет связи с собеседником — дождитесь «на связи»"), true);
    }
    return;
  }
  if (!service_.can_start_call(key)) {
    showToast(QStringLiteral("Нет права открывать комнату (нужна роль ведущего)"), true);
    return;
  }
  if (video && CallVideoIo::listCameraDevices().isEmpty()) {
    showToast(QStringLiteral("Камера не найдена — аудиозвонок"), false);
    video = false;
  }
  struct Ctx {
    NodeController* self;
    bool video;
  };
  auto* ctx = new Ctx {this, video};
  const bool need_camera = video && !CallVideoIo::listCameraDevices().isEmpty();
  nyx_android::request_call_permissions(
      need_camera,
      [](bool mic_ok, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        bool want_video = c->video;
        delete c;
        if (!mic_ok) {
          self->showToast(QStringLiteral("Нужен доступ к микрофону"), true);
          return;
        }
        if (want_video && !cam_ok) {
          self->showToast(QStringLiteral("Нет доступа к камере — аудиозвонок"), false);
          want_video = false;
        }
        if (!self->service_.start_call(want_video, self->active_chat_key_.toStdString())) {
          self->showToast(QStringLiteral("Не удалось начать звонок"), true);
        }
      },
      ctx);
}

void NodeController::acceptCall() {
  call_ui_.answering_call_ = true;
#if defined(Q_OS_ANDROID)

  nyx_android::stop_ringtone();
#endif
  struct Ctx {
    NodeController* self;
  };
  auto* ctx = new Ctx {this};
  const bool video = callVideo();
  const bool need_camera = video && !CallVideoIo::listCameraDevices().isEmpty();
  nyx_android::request_call_permissions(
      need_camera,
      [](bool mic_ok, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        delete c;
        if (!mic_ok) {
          self->call_ui_.answering_call_ = false;
          self->call_ui_.last_call_notify_key_.clear();
          self->syncCallNotifications();
          self->showToast(QStringLiteral("Нужен доступ к микрофону"), true);
          return;
        }
        if (self->callVideo() && !cam_ok) {
          self->showToast(QStringLiteral("Нет доступа к камере — только приём видео"), false);
        }
        if (!self->service_.accept_call()) {
          self->call_ui_.answering_call_ = false;
          self->call_ui_.last_call_notify_key_.clear();
          self->syncCallNotifications();
          self->showToast(QStringLiteral("Не удалось войти в комнату"), true);
        }
      },
      ctx);
}

void NodeController::rejectCall() {
  service_.reject_call();
}

void NodeController::hangupCall() {
  service_.hangup_call();
}

void NodeController::syncCallAudio() {
  const auto st = service_.call_state();
  const bool video = service_.call_mode() == nyx::CallMode::AudioVideo;
  if (st == nyx::CallState::Active) {
    if (call_ui_.call_audio_.micTestActive())
      call_ui_.call_audio_.stopMicLevelTest();
#if defined(Q_OS_ANDROID)
    nyx_android::set_voip_audio_mode(true);
    nyx_android::set_speakerphone(call_ui_.call_speakerphone_);
#endif
    const bool mic_muted = service_.call_mic_muted();

    call_ui_.call_audio_.setMuted(mic_muted);
    call_ui_.call_audio_.setSendFn([this](const std::vector<uint8_t>& packet) {
      const bool ok = service_.send_call_media(
          nyx::CallMediaType::Opus, packet, call_ui_.call_audio_.localVoiceLevel());
      if (!ok) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        if (now - last_send_fail_toast_ms_ > 4000 && now - call_ui_.call_media_started_ms_ > 1500) {
          last_send_fail_toast_ms_ = now;
          QMetaObject::invokeMethod(
              this,
              [this]() { showToast(QStringLiteral("Аудио не уходит (нет канала)"), true); },
              Qt::QueuedConnection);
        }
      }
      return ok;
    });
    call_ui_.call_media_started_ms_ = QDateTime::currentMSecsSinceEpoch();
    call_ui_.call_audio_.start();
    if (video) {
      const bool small_field =
          service_.call_is_field_room() && service_.call_participants().size() <= 2;
      call_ui_.call_video_.setTransmitEnabled(!service_.call_is_field_room() || small_field ||
                                              call_ui_.call_audio_.localVoiceActive());
      call_ui_.call_video_.setSendFn([this](const QByteArray& frag) {
        const nyx::ByteBuffer buf(frag.begin(), frag.end());
        return service_.send_call_media(nyx::CallMediaType::Video, buf);
      });
      if (!call_ui_.call_video_slots_wired_) {
        call_ui_.call_video_slots_wired_ = true;
        connect(
            &call_ui_.call_video_,
            &CallVideoIo::remoteFrameChanged,
            this,
            [this](const QString& peerId) {
              const QImage img = call_ui_.call_video_.peerFrame(peerId);
              if (img.isNull())
                return;
              if (call_ui_.call_frames_) {
                call_ui_.call_frames_->setRemote(peerId, img);
                call_ui_.call_frames_->setPrimaryRemoteKey(call_ui_.call_video_.focusedPeerId());
              }
              if (peerId == call_ui_.call_video_.focusedPeerId() ||
                  call_ui_.call_video_.focusedPeerId().isEmpty()) {
                ++call_ui_.call_frame_epoch_;
                call_ui_.call_remote_frame_url_ = QUrl(
                    QStringLiteral("image://nyxcall/remote/%1").arg(call_ui_.call_frame_epoch_));
                emit callRemoteFrameChanged();
              }
              emit callVideoPeersChanged();
            });
        connect(&call_ui_.call_video_, &CallVideoIo::localFrameChanged, this, [this]() {
          const QImage img = call_ui_.call_video_.lastLocalFrame();
          if (call_ui_.call_frames_)
            call_ui_.call_frames_->setLocal(img);
          ++call_ui_.call_frame_epoch_;
          if (img.isNull()) {
            call_ui_.call_local_frame_url_.clear();
          } else {
            call_ui_.call_local_frame_url_ =
                QUrl(QStringLiteral("image://nyxcall/local/%1").arg(call_ui_.call_frame_epoch_));
          }
          emit callLocalFrameChanged();
        });
        connect(&call_ui_.call_video_,
                &CallVideoIo::videoPeersChanged,
                this,
                &NodeController::callVideoPeersChanged);
        connect(&call_ui_.call_video_, &CallVideoIo::cameraChanged, this, [this]() {
          saveMediaDevicePrefs();
          emit mediaDevicesChanged();
          emit callChanged();
        });
      }
      if (!call_ui_.call_video_.running()) {
#if defined(Q_OS_ANDROID)

        service_.set_call_camera_on(true);
        call_ui_.call_video_.setCameraEnabled(true);
        call_ui_.call_video_.start();
        emit callChanged();
        emit mediaDevicesChanged();
        QTimer::singleShot(800, this, [this]() {
          if (service_.call_state() != nyx::CallState::Active)
            return;
          if (service_.call_mode() != nyx::CallMode::AudioVideo)
            return;
          if (!call_ui_.call_video_.running())
            return;
          if (call_ui_.call_video_.capturing())
            return;
          showToast(QStringLiteral("Камера недоступна — только приём видео"), false);
          service_.set_call_camera_on(false);
          emit callChanged();
          emit mediaDevicesChanged();
        });
#else
        service_.set_call_camera_on(true);
        call_ui_.call_video_.setCameraEnabled(true);
        call_ui_.call_video_.start();
        emit callChanged();
        emit mediaDevicesChanged();
        if (!call_ui_.call_video_.capturing()) {
          QTimer::singleShot(500, this, [this]() {
            if (service_.call_state() != nyx::CallState::Active)
              return;
            if (service_.call_mode() != nyx::CallMode::AudioVideo)
              return;
            if (!call_ui_.call_video_.running())
              return;
            if (call_ui_.call_video_.capturing())
              return;
            call_ui_.call_video_.setCameraEnabled(true);
            emit callChanged();
            emit mediaDevicesChanged();
            if (!call_ui_.call_video_.capturing()) {
              showToast(QStringLiteral("Камера недоступна — только приём видео"), false);
            }
          });
        }
#endif
      }
    } else if (call_ui_.call_video_slots_wired_ || call_ui_.call_video_.running()) {
      disconnect(&call_ui_.call_video_, nullptr, this, nullptr);
      call_ui_.call_video_slots_wired_ = false;
      call_ui_.call_video_.stop();
      if (call_ui_.call_frames_)
        call_ui_.call_frames_->clear();
      call_ui_.call_remote_frame_url_.clear();
      call_ui_.call_local_frame_url_.clear();
      emit callRemoteFrameChanged();
      emit callLocalFrameChanged();
    }
  } else {
#if defined(Q_OS_ANDROID)
    nyx_android::set_voip_audio_mode(false);
#endif
    disconnect(&call_ui_.call_video_, nullptr, this, nullptr);
    call_ui_.call_video_slots_wired_ = false;
    call_ui_.call_audio_.stop();
    call_ui_.call_video_.stop();
    if (call_ui_.call_frames_)
      call_ui_.call_frames_->clear();
    call_ui_.call_remote_frame_url_.clear();
    call_ui_.call_local_frame_url_.clear();
    call_ui_.call_frame_epoch_ = 0;
    emit callRemoteFrameChanged();
    emit callLocalFrameChanged();
    emit callVideoPeersChanged();
    emit callChanged();
  }
}

QUrl NodeController::callRemoteFrameUrl() const {
  return call_ui_.call_remote_frame_url_;
}

QUrl NodeController::callLocalFrameUrl() const {
  return call_ui_.call_local_frame_url_;
}

bool NodeController::callCanSwitchCamera() const {
  return call_ui_.call_video_.canSwitchCamera();
}

void NodeController::setCallFrameProvider(CallFrameProvider* provider) {
  call_ui_.call_frames_ = provider;
}

void NodeController::loadMediaDevicePrefs() {
  QSettings s;
  s.beginGroup(QStringLiteral("callMedia"));
  const QString cam = s.value(QStringLiteral("cameraId")).toString();
  const QString ain = s.value(QStringLiteral("audioInputId")).toString();
  const QString aout = s.value(QStringLiteral("audioOutputId")).toString();
  call_ui_.call_speakerphone_ = s.value(QStringLiteral("speakerphone"), true).toBool();
  s.endGroup();
  if (!cam.isEmpty())
    call_ui_.call_video_.setPreferredCameraId(cam);
  if (!ain.isEmpty())
    call_ui_.call_audio_.setPreferredInputId(ain);
  if (!aout.isEmpty())
    call_ui_.call_audio_.setPreferredOutputId(aout);
}

void NodeController::saveMediaDevicePrefs() const {
  QSettings s;
  s.beginGroup(QStringLiteral("callMedia"));
  s.setValue(QStringLiteral("cameraId"), call_ui_.call_video_.preferredCameraId());
  s.setValue(QStringLiteral("audioInputId"), call_ui_.call_audio_.preferredInputId());
  s.setValue(QStringLiteral("audioOutputId"), call_ui_.call_audio_.preferredOutputId());
  s.setValue(QStringLiteral("speakerphone"), call_ui_.call_speakerphone_);
  s.endGroup();
}

void NodeController::refreshMediaDevices() {
  emit mediaDevicesChanged();
}

float NodeController::audioTestLevel() const {
  return call_ui_.call_audio_.micLevel();
}

bool NodeController::audioTestActive() const {
  return call_ui_.call_audio_.micTestActive();
}

void NodeController::startMicTest() {
  if (service_.call_state() == nyx::CallState::Active ||
      service_.call_state() == nyx::CallState::Outgoing ||
      service_.call_state() == nyx::CallState::Ringing ||
      service_.call_state() == nyx::CallState::Incoming) {
    showToast(QStringLiteral("Сначала завершите звонок"), true);
    return;
  }
#if defined(Q_OS_ANDROID)
  nyx_android::request_call_permissions(
      false,
      [](bool mic_ok, bool, void* ctx) {
        auto* self = static_cast<NodeController*>(ctx);
        if (!mic_ok) {
          QMetaObject::invokeMethod(
              self,
              [self]() { self->showToast(QStringLiteral("Нет доступа к микрофону"), true); },
              Qt::QueuedConnection);
          return;
        }
        QMetaObject::invokeMethod(
            self,
            [self]() {
              if (!self->call_ui_.call_audio_.startMicLevelTest())
                self->showToast(QStringLiteral("Не удалось открыть микрофон"), true);
            },
            Qt::QueuedConnection);
      },
      this);
#else
  if (!call_ui_.call_audio_.startMicLevelTest())
    showToast(QStringLiteral("Не удалось открыть микрофон"), true);
#endif
}

void NodeController::stopAudioTest() {
  call_ui_.call_audio_.stopMicLevelTest();
}

void NodeController::playSpeakerTest() {
  if (service_.call_state() == nyx::CallState::Active) {
    showToast(QStringLiteral("Сначала завершите звонок"), true);
    return;
  }
  call_ui_.call_audio_.playSpeakerTestTone();
}

QVariantList NodeController::cameraDeviceList() const {
  return CallVideoIo::listCameraDevices();
}

QVariantList NodeController::audioInputDeviceList() const {
  return CallAudioIo::listInputDevices();
}

QVariantList NodeController::audioOutputDeviceList() const {
  return CallAudioIo::listOutputDevices();
}

QString NodeController::selectedCameraId() const {
  return call_ui_.call_video_.preferredCameraId();
}

QString NodeController::selectedAudioInputId() const {
  return call_ui_.call_audio_.preferredInputId();
}

QString NodeController::selectedAudioOutputId() const {
  return call_ui_.call_audio_.preferredOutputId();
}

void NodeController::setSelectedCameraId(const QString& id) {
  call_ui_.call_video_.setPreferredCameraId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
  emit callChanged();
}

void NodeController::setSelectedAudioInputId(const QString& id) {
  call_ui_.call_audio_.setPreferredInputId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
}

void NodeController::setSelectedAudioOutputId(const QString& id) {
  call_ui_.call_audio_.setPreferredOutputId(id);
  saveMediaDevicePrefs();
  emit mediaDevicesChanged();
}

QString NodeController::resolveCallPeerName(const QString& peerIdHex) const {
  const QString uid = peerIdHex.trimmed().toLower();
  if (uid.isEmpty() || uid == QLatin1String("direct"))
    return callTitle();
  for (const QVariant& v : contact_list_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid)
      return m.value(QStringLiteral("nickname")).toString();
  }
  for (const QVariant& v : field_info_members_) {
    const QVariantMap m = v.toMap();
    if (m.value(QStringLiteral("userId")).toString().toLower() == uid)
      return m.value(QStringLiteral("nickname")).toString();
  }
  return uid.left(8);
}

QVariantList NodeController::callVideoPeers() const {
  QVariantList out;
  for (const QString& id : call_ui_.call_video_.videoPeerIds()) {
    QVariantMap row;
    row.insert(QStringLiteral("userId"), id);
    row.insert(QStringLiteral("nickname"), resolveCallPeerName(id));
    row.insert(QStringLiteral("focused"), id == call_ui_.call_video_.focusedPeerId());
    out.append(row);
  }
  return out;
}

QVariantList NodeController::callRosterPeers() const {
  if (!callIsFieldRoom())
    return {};
  QVariantList video = callVideoPeers();
  if (!video.isEmpty())
    return video;
  QVariantList out;
  const auto self = service_.profile().public_key;
  for (const auto& participant : service_.call_participants()) {
    if (participant == self)
      continue;
    const QString id = QString::fromStdString(nyx::to_hex(participant.data(), participant.size()));
    QVariantMap row;
    row.insert(QStringLiteral("userId"), id);
    row.insert(QStringLiteral("nickname"), resolveCallPeerName(id));
    row.insert(QStringLiteral("focused"), id == call_ui_.call_video_.focusedPeerId());
    out.append(row);
  }
  if (!out.isEmpty())
    return out;
  for (const QVariant& v : field_info_members_) {
    const QVariantMap m = v.toMap();
    QVariantMap row;
    row.insert(QStringLiteral("userId"), m.value(QStringLiteral("userId")));
    row.insert(QStringLiteral("nickname"), m.value(QStringLiteral("nickname")));
    row.insert(QStringLiteral("focused"), false);
    out.append(row);
  }
  return out;
}

bool NodeController::callMicMuted() const {
  return service_.call_mic_muted();
}

void NodeController::setCallMicMuted(bool muted) {
  service_.set_call_mic_muted(muted);
  call_ui_.call_audio_.setMuted(muted);
  emit callChanged();
}

void NodeController::toggleCallMicMuted() {
  setCallMicMuted(!callMicMuted());
}

bool NodeController::callCameraOn() const {
  return service_.call_camera_on() && call_ui_.call_video_.cameraEnabled();
}

void NodeController::setCallCameraOn(bool on) {
  if (!on) {
    service_.set_call_camera_on(false);
    call_ui_.call_video_.setCameraEnabled(false);
    if (call_ui_.call_frames_)
      call_ui_.call_frames_->setLocal(QImage());
    call_ui_.call_local_frame_url_.clear();
    emit callLocalFrameChanged();
    emit callChanged();
    return;
  }
#if defined(Q_OS_ANDROID)
  struct Ctx {
    NodeController* self;
  };
  auto* ctx = new Ctx {this};
  nyx_android::request_call_permissions(
      true,
      [](bool, bool cam_ok, void* p) {
        auto* c = static_cast<Ctx*>(p);
        NodeController* self = c->self;
        delete c;
        if (!cam_ok) {
          self->showToast(QStringLiteral("Нужен доступ к камере"), true);
          self->service_.set_call_camera_on(false);
          self->call_ui_.call_video_.setCameraEnabled(false);
          emit self->callChanged();
          return;
        }
        self->service_.set_call_camera_on(true);
        self->call_ui_.call_video_.setCameraEnabled(true);
        emit self->callChanged();
      },
      ctx);
#else
  service_.set_call_camera_on(true);
  call_ui_.call_video_.setCameraEnabled(true);
  emit callChanged();
#endif
}

void NodeController::toggleCallCamera() {
  setCallCameraOn(!callCameraOn());
}

bool NodeController::callSpeakerphone() const {
  return call_ui_.call_speakerphone_;
}

void NodeController::setCallSpeakerphone(bool on) {
  call_ui_.call_speakerphone_ = on;
#if defined(Q_OS_ANDROID)
  nyx_android::set_speakerphone(on);
#endif
  saveMediaDevicePrefs();
  emit callChanged();
}

void NodeController::toggleCallSpeakerphone() {
  setCallSpeakerphone(!callSpeakerphone());
}

void NodeController::switchCallCamera() {
  if (!call_ui_.call_video_.switchCamera()) {
    showToast(QStringLiteral("Другая камера недоступна"), true);
    return;
  }
  saveMediaDevicePrefs();
  emit callChanged();
  emit mediaDevicesChanged();
}

void NodeController::setCallFocusedPeer(const QString& peerIdHex) {
  const QString id = peerIdHex.trimmed().toLower();
  if (id.isEmpty())
    return;
  call_ui_.manual_call_focus_ = id;
  call_ui_.manual_call_focus_until_ms_ = QDateTime::currentMSecsSinceEpoch() + 10000;
  call_ui_.call_video_.setFocusedPeerId(id);
  if (call_ui_.call_frames_)
    call_ui_.call_frames_->setPrimaryRemoteKey(id);
  const QImage img = call_ui_.call_video_.peerFrame(id);
  if (img.isNull()) {
    emit callVideoPeersChanged();
    return;
  }
  if (call_ui_.call_frames_)
    call_ui_.call_frames_->setRemote(id, img);
  ++call_ui_.call_frame_epoch_;
  call_ui_.call_remote_frame_url_ =
      QUrl(QStringLiteral("image://nyxcall/remote/%1").arg(call_ui_.call_frame_epoch_));
  emit callRemoteFrameChanged();
  emit callVideoPeersChanged();
}
