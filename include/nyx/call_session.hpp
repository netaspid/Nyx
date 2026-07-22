#pragma once

/** @file call_session.hpp
 *  Локальная FSM звонка (только сигналинг; медиа подключается отдельно).
 */

#include "nyx/call_proto.hpp"

#include <string>

namespace nyx {

enum class CallState : uint8_t {
  Idle = 0,
  Outgoing = 1,
  Incoming = 2,
  Ringing = 3,
  Active = 4,
  Ended = 5,
};

struct CallSession {
  CallState state = CallState::Idle;
  CallId call_id{};
  CallMode mode = CallMode::Audio;
  CallScope scope = CallScope::Direct;
  UserId remote_or_group{};
  bool local_mic_muted = false;
  bool local_camera_on = false;
  std::string end_reason;

  bool idle() const { return state == CallState::Idle || state == CallState::Ended; }

  bool start_outgoing(CallMode m, CallScope s, const UserId& target, CallId id = {});
  bool open_field_room(CallMode m, const UserId& group, CallId id = {});
  bool on_invite(const CallInviteMessage& msg);
  bool on_ringing(const CallRingingMessage& msg);
  bool accept(CallMode m = CallMode::Audio);
  bool reject(CallRejectReason reason = CallRejectReason::Declined);
  bool on_accept(const CallAcceptMessage& msg);
  bool on_reject(const CallRejectMessage& msg);
  bool hangup(CallHangupReason reason = CallHangupReason::Normal);
  bool on_hangup(const CallHangupMessage& msg);
  bool on_update(const CallUpdateMessage& msg);
  bool on_peer_leave(const CallId& id);
  void reset();
};

}  // namespace nyx
