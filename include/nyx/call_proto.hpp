#pragma once

/** @file call_proto.hpp
 *  Call signaling on kChatStream (CallKind 0x60+).
 *  Media flows separately on kRealtimeStream.
 */

#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

constexpr std::size_t kCallIdSize = 16;
using CallId = std::array<uint8_t, kCallIdSize>;
/** Upper bound of mesh conference participants. */
constexpr std::size_t kMaxCallParticipants = 20;

enum class CallKind : uint8_t {
  Invite = 0x60,
  Ringing = 0x61,
  Accept = 0x62,
  Reject = 0x63,
  Hangup = 0x64,
  Update = 0x65,
  Roster = 0x66,
  PeerIntro = 0x67,
  PeerGone = 0x68,
  Endpoint = 0x69,
  LeaveAck = 0x6A,
  RelayCandidate = 0x6B,
  RelaySet = 0x6C,
};

enum class CallMode : uint8_t { Audio = 1, AudioVideo = 2 };

enum class CallScope : uint8_t { Direct = 1, Field = 2 };

enum class CallRejectReason : uint8_t {
  Declined = 1,
  Busy = 2,
  Unsupported = 3,
  Timeout = 4,
};

enum class CallHangupReason : uint8_t {
  Normal = 1,
  Error = 2,
  Replaced = 3,
  HubClosed = 4,
};

struct CallInviteMessage {
  CallId call_id{};
  CallMode mode = CallMode::Audio;
  CallScope scope = CallScope::Direct;
  /** DM: peer id; Field: group id as 32 bytes. */
  UserId group_or_peer{};
  std::string sdp_lite;

  ByteBuffer encode() const;
  static std::optional<CallInviteMessage> decode(const ByteBuffer& data);
};

struct CallRingingMessage {
  CallId call_id{};
  ByteBuffer encode() const;
  static std::optional<CallRingingMessage> decode(const ByteBuffer& data);
};

struct CallAcceptMessage {
  CallId call_id{};
  CallMode mode = CallMode::Audio;
  std::string sdp_lite;

  ByteBuffer encode() const;
  static std::optional<CallAcceptMessage> decode(const ByteBuffer& data);
};

struct CallRejectMessage {
  CallId call_id{};
  CallRejectReason reason = CallRejectReason::Declined;

  ByteBuffer encode() const;
  static std::optional<CallRejectMessage> decode(const ByteBuffer& data);
};

struct CallHangupMessage {
  CallId call_id{};
  CallHangupReason reason = CallHangupReason::Normal;

  ByteBuffer encode() const;
  static std::optional<CallHangupMessage> decode(const ByteBuffer& data);
};

struct CallUpdateMessage {
  CallId call_id{};
  bool mic_muted = false;
  bool camera_on = false;
  bool screen_share = false;

  ByteBuffer encode() const;
  static std::optional<CallUpdateMessage> decode(const ByteBuffer& data);
};

struct CallPeerEndpoint {
  UserId user_id{};
  std::string host;
  uint16_t port = 0;
};

struct CallRosterMessage {
  CallId call_id{};
  std::vector<UserId> participants;

  ByteBuffer encode() const;
  static std::optional<CallRosterMessage> decode(const ByteBuffer& data);
};

struct CallPeerIntroMessage {
  CallId call_id{};
  CallPeerEndpoint peer;

  ByteBuffer encode() const;
  static std::optional<CallPeerIntroMessage> decode(const ByteBuffer& data);
};

struct CallPeerGoneMessage {
  CallId call_id{};
  UserId user_id{};

  ByteBuffer encode() const;
  static std::optional<CallPeerGoneMessage> decode(const ByteBuffer& data);
};

/** Local UDP endpoint for mesh media. */
struct CallEndpointMessage {
  CallId call_id{};
  CallPeerEndpoint self;

  ByteBuffer encode() const;
  static std::optional<CallEndpointMessage> decode(const ByteBuffer& data);
};

struct CallLeaveAckMessage {
  CallId call_id{};
  UserId user_id{};

  ByteBuffer encode() const;
  static std::optional<CallLeaveAckMessage> decode(const ByteBuffer& data);
};

struct CallRelayCandidateMessage {
  CallId call_id{};
  UserId user_id{};
  uint16_t score = 0;

  ByteBuffer encode() const;
  static std::optional<CallRelayCandidateMessage> decode(const ByteBuffer& data);
};

struct CallRelaySetMessage {
  CallId call_id{};
  uint32_t epoch = 0;
  std::vector<UserId> relays;

  ByteBuffer encode() const;
  static std::optional<CallRelaySetMessage> decode(const ByteBuffer& data);
};

bool is_call_frame(const ByteBuffer& data);
CallId generate_call_id();
std::string call_id_hex(const CallId& id);
bool call_id_from_hex(const std::string& hex, CallId& out);

}  // namespace nyx
