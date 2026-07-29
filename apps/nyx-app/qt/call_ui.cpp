#include "call_ui.hpp"

#include "node_controller.hpp"

QString CallUi::callState() const {
  return host_ ? host_->callState() : QString();
}
QString CallUi::callTitle() const {
  return host_ ? host_->callTitle() : QString();
}
bool CallUi::callVideo() const {
  return host_ ? host_->callVideo() : false;
}
bool CallUi::canStartCall() const {
  return host_ ? host_->canStartCall() : false;
}
bool CallUi::callIsFieldRoom() const {
  return host_ ? host_->callIsFieldRoom() : false;
}
bool CallUi::callMicMuted() const {
  return host_ ? host_->callMicMuted() : false;
}
void CallUi::setCallMicMuted(bool muted) {
  if (host_)
    host_->setCallMicMuted(muted);
}
bool CallUi::callCameraOn() const {
  return host_ ? host_->callCameraOn() : false;
}
void CallUi::setCallCameraOn(bool on) {
  if (host_)
    host_->setCallCameraOn(on);
}
bool CallUi::callSpeakerphone() const {
  return call_speakerphone_;
}
void CallUi::setCallSpeakerphone(bool on) {
  if (host_)
    host_->setCallSpeakerphone(on);
}
float CallUi::audioTestLevel() const {
  return host_ ? host_->audioTestLevel() : 0.f;
}
bool CallUi::audioTestActive() const {
  return host_ ? host_->audioTestActive() : false;
}
bool CallUi::callCanSwitchCamera() const {
  return host_ ? host_->callCanSwitchCamera() : false;
}
QVariantList CallUi::callVideoPeers() const {
  return host_ ? host_->callVideoPeers() : QVariantList();
}
QVariantList CallUi::callRosterPeers() const {
  return host_ ? host_->callRosterPeers() : QVariantList();
}
QVariantList CallUi::cameraDeviceList() const {
  return host_ ? host_->cameraDeviceList() : QVariantList();
}
QVariantList CallUi::audioInputDeviceList() const {
  return host_ ? host_->audioInputDeviceList() : QVariantList();
}
QVariantList CallUi::audioOutputDeviceList() const {
  return host_ ? host_->audioOutputDeviceList() : QVariantList();
}
QString CallUi::selectedCameraId() const {
  return host_ ? host_->selectedCameraId() : QString();
}
QString CallUi::selectedAudioInputId() const {
  return host_ ? host_->selectedAudioInputId() : QString();
}
QString CallUi::selectedAudioOutputId() const {
  return host_ ? host_->selectedAudioOutputId() : QString();
}
void CallUi::setSelectedCameraId(const QString& id) {
  if (host_)
    host_->setSelectedCameraId(id);
}
void CallUi::setSelectedAudioInputId(const QString& id) {
  if (host_)
    host_->setSelectedAudioInputId(id);
}
void CallUi::setSelectedAudioOutputId(const QString& id) {
  if (host_)
    host_->setSelectedAudioOutputId(id);
}

void CallUi::startCall(bool video) {
  if (host_)
    host_->startCall(video);
}
void CallUi::acceptCall() {
  if (host_)
    host_->acceptCall();
}
void CallUi::rejectCall() {
  if (host_)
    host_->rejectCall();
}
void CallUi::hangupCall() {
  if (host_)
    host_->hangupCall();
}
void CallUi::switchCallCamera() {
  if (host_)
    host_->switchCallCamera();
}
void CallUi::setCallFocusedPeer(const QString& peerIdHex) {
  if (host_)
    host_->setCallFocusedPeer(peerIdHex);
}
void CallUi::toggleCallMicMuted() {
  if (host_)
    host_->toggleCallMicMuted();
}
void CallUi::toggleCallCamera() {
  if (host_)
    host_->toggleCallCamera();
}
void CallUi::toggleCallSpeakerphone() {
  if (host_)
    host_->toggleCallSpeakerphone();
}
void CallUi::refreshMediaDevices() {
  if (host_)
    host_->refreshMediaDevices();
}
void CallUi::startMicTest() {
  if (host_)
    host_->startMicTest();
}
void CallUi::stopAudioTest() {
  if (host_)
    host_->stopAudioTest();
}
void CallUi::playSpeakerTest() {
  if (host_)
    host_->playSpeakerTest();
}
