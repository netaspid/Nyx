#include "nyx/call_session.hpp"

namespace nyx {

namespace {

bool call_id_zero(const CallId& id) {
  for (uint8_t b : id) {
    if (b != 0) return false;
  }
  return true;
}

}  // namespace

bool CallSession::start_outgoing(CallMode m, CallScope s, const UserId& target, CallId id) {
  if (!idle()) return false;
  call_id = call_id_zero(id) ? generate_call_id() : id;
  mode = m;
  scope = s;
  remote_or_group = target;
  local_camera_on = (m == CallMode::AudioVideo);
  local_mic_muted = false;
  end_reason.clear();
  state = CallState::Outgoing;
  return true;
}

bool CallSession::open_field_room(CallMode m, const UserId& group, CallId id) {
  if (!idle()) return false;
  call_id = call_id_zero(id) ? generate_call_id() : id;
  mode = m;
  scope = CallScope::Field;
  remote_or_group = group;
  local_camera_on = (m == CallMode::AudioVideo);
  local_mic_muted = false;
  end_reason.clear();
  state = CallState::Active;
  return true;
}

bool CallSession::on_invite(const CallInviteMessage& msg) {
  if (!idle()) return false;
  call_id = msg.call_id;
  mode = msg.mode;
  scope = msg.scope;
  remote_or_group = msg.group_or_peer;
  local_camera_on = false;
  local_mic_muted = false;
  end_reason.clear();
  state = CallState::Incoming;
  return true;
}

bool CallSession::on_ringing(const CallRingingMessage& msg) {
  if (state != CallState::Outgoing || msg.call_id != call_id) return false;
  state = CallState::Ringing;
  return true;
}

bool CallSession::accept(CallMode m) {
  if (state != CallState::Incoming) return false;
  mode = m;
  local_camera_on = (m == CallMode::AudioVideo);
  state = CallState::Active;
  return true;
}

bool CallSession::reject(CallRejectReason /*reason*/) {
  if (state != CallState::Incoming) return false;
  end_reason = "reject";
  state = CallState::Ended;
  return true;
}

bool CallSession::on_accept(const CallAcceptMessage& msg) {
  if (msg.call_id != call_id) return false;
  // Поле: комната уже Active — участник присоединился.
  if (scope == CallScope::Field && state == CallState::Active) {
    mode = msg.mode;
    return true;
  }
  if (state != CallState::Outgoing && state != CallState::Ringing) return false;
  mode = msg.mode;
  state = CallState::Active;
  return true;
}

bool CallSession::on_reject(const CallRejectMessage& msg) {
  if (msg.call_id != call_id) return false;
  if (scope == CallScope::Field && state == CallState::Active) {
    // Хаб отклонил старт (нет роли) — закрываем комнату у инициатора.
    if (msg.reason == CallRejectReason::Unsupported) {
      end_reason = "unsupported";
      state = CallState::Ended;
      return true;
    }
    return false;
  }
  if (state != CallState::Outgoing && state != CallState::Ringing) return false;
  end_reason = "rejected";
  state = CallState::Ended;
  return true;
}

bool CallSession::hangup(CallHangupReason /*reason*/) {
  if (state != CallState::Active && state != CallState::Outgoing && state != CallState::Ringing &&
      state != CallState::Incoming)
    return false;
  end_reason = "hangup";
  state = CallState::Ended;
  return true;
}

bool CallSession::on_hangup(const CallHangupMessage& msg) {
  if (idle() || msg.call_id != call_id) return false;
  end_reason = "remote_hangup";
  state = CallState::Ended;
  return true;
}

bool CallSession::on_update(const CallUpdateMessage& msg) {
  return state == CallState::Active && msg.call_id == call_id;
}

bool CallSession::on_peer_leave(const CallId& id) {
  return state == CallState::Active && id == call_id;
}

void CallSession::reset() {
  state = CallState::Idle;
  call_id = {};
  mode = CallMode::Audio;
  scope = CallScope::Direct;
  remote_or_group = {};
  local_mic_muted = false;
  local_camera_on = false;
  end_reason.clear();
}

}  // namespace nyx
