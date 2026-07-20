#include "nyx/call_proto.hpp"

#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>

namespace nyx {

namespace {

constexpr std::size_t kMaxSdpLite = 2048;
constexpr std::size_t kMaxHost = 64;
constexpr std::size_t kMaxRoster = kMaxCallParticipants;

bool read_str(const ByteBuffer& data, std::size_t& off, std::size_t max_len, std::string& out) {
  if (off + 2 > data.size()) return false;
  const uint16_t len = read_u16_le(data.data() + off);
  off += 2;
  if (len > max_len || off + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + off), len);
  off += len;
  return true;
}

void write_str(ByteBuffer& out, const std::string& s) {
  write_u16_le(out, static_cast<uint16_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

bool read_call_id(const ByteBuffer& data, std::size_t& off, CallId& id) {
  if (off + kCallIdSize > data.size()) return false;
  std::memcpy(id.data(), data.data() + off, kCallIdSize);
  off += kCallIdSize;
  return true;
}

void write_call_id(ByteBuffer& out, const CallId& id) {
  out.insert(out.end(), id.begin(), id.end());
}

bool read_user_id(const ByteBuffer& data, std::size_t& off, UserId& id) {
  if (off + kPublicKeySize > data.size()) return false;
  std::memcpy(id.data(), data.data() + off, kPublicKeySize);
  off += kPublicKeySize;
  return true;
}

void write_user_id(ByteBuffer& out, const UserId& id) {
  out.insert(out.end(), id.begin(), id.end());
}

}  // namespace

bool is_call_frame(const ByteBuffer& data) {
  if (data.empty()) return false;
  const uint8_t b = data[0];
  return b >= static_cast<uint8_t>(CallKind::Invite) &&
         b <= static_cast<uint8_t>(CallKind::Endpoint);
}

CallId generate_call_id() {
  CallId id{};
  random_bytes(id.data(), id.size());
  return id;
}

std::string call_id_hex(const CallId& id) { return to_hex(id.data(), id.size()); }

bool call_id_from_hex(const std::string& hex, CallId& out) {
  std::vector<uint8_t> raw;
  if (!from_hex(hex, raw) || raw.size() != kCallIdSize) return false;
  std::memcpy(out.data(), raw.data(), kCallIdSize);
  return true;
}

ByteBuffer CallInviteMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Invite));
  write_call_id(out, call_id);
  out.push_back(static_cast<uint8_t>(mode));
  out.push_back(static_cast<uint8_t>(scope));
  write_user_id(out, group_or_peer);
  write_str(out, sdp_lite);
  return out;
}

std::optional<CallInviteMessage> CallInviteMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kCallIdSize + 2 + kPublicKeySize + 2 ||
      data[0] != static_cast<uint8_t>(CallKind::Invite))
    return std::nullopt;
  CallInviteMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  m.mode = static_cast<CallMode>(data[off++]);
  m.scope = static_cast<CallScope>(data[off++]);
  if (m.mode != CallMode::Audio && m.mode != CallMode::AudioVideo) return std::nullopt;
  if (m.scope != CallScope::Direct && m.scope != CallScope::Field) return std::nullopt;
  if (!read_user_id(data, off, m.group_or_peer)) return std::nullopt;
  if (!read_str(data, off, kMaxSdpLite, m.sdp_lite)) return std::nullopt;
  return m;
}

ByteBuffer CallRingingMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Ringing));
  write_call_id(out, call_id);
  return out;
}

std::optional<CallRingingMessage> CallRingingMessage::decode(const ByteBuffer& data) {
  if (data.size() != 1 + kCallIdSize || data[0] != static_cast<uint8_t>(CallKind::Ringing))
    return std::nullopt;
  CallRingingMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  return m;
}

ByteBuffer CallAcceptMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Accept));
  write_call_id(out, call_id);
  out.push_back(static_cast<uint8_t>(mode));
  write_str(out, sdp_lite);
  return out;
}

std::optional<CallAcceptMessage> CallAcceptMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kCallIdSize + 1 + 2 ||
      data[0] != static_cast<uint8_t>(CallKind::Accept))
    return std::nullopt;
  CallAcceptMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  m.mode = static_cast<CallMode>(data[off++]);
  if (m.mode != CallMode::Audio && m.mode != CallMode::AudioVideo) return std::nullopt;
  if (!read_str(data, off, kMaxSdpLite, m.sdp_lite)) return std::nullopt;
  return m;
}

ByteBuffer CallRejectMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Reject));
  write_call_id(out, call_id);
  out.push_back(static_cast<uint8_t>(reason));
  return out;
}

std::optional<CallRejectMessage> CallRejectMessage::decode(const ByteBuffer& data) {
  if (data.size() != 1 + kCallIdSize + 1 || data[0] != static_cast<uint8_t>(CallKind::Reject))
    return std::nullopt;
  CallRejectMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  m.reason = static_cast<CallRejectReason>(data[off]);
  return m;
}

ByteBuffer CallHangupMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Hangup));
  write_call_id(out, call_id);
  out.push_back(static_cast<uint8_t>(reason));
  return out;
}

std::optional<CallHangupMessage> CallHangupMessage::decode(const ByteBuffer& data) {
  if (data.size() != 1 + kCallIdSize + 1 || data[0] != static_cast<uint8_t>(CallKind::Hangup))
    return std::nullopt;
  CallHangupMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  m.reason = static_cast<CallHangupReason>(data[off]);
  return m;
}

ByteBuffer CallUpdateMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Update));
  write_call_id(out, call_id);
  uint8_t flags = 0;
  if (mic_muted) flags |= 0x01;
  if (camera_on) flags |= 0x02;
  if (screen_share) flags |= 0x04;
  out.push_back(flags);
  return out;
}

std::optional<CallUpdateMessage> CallUpdateMessage::decode(const ByteBuffer& data) {
  if (data.size() != 1 + kCallIdSize + 1 || data[0] != static_cast<uint8_t>(CallKind::Update))
    return std::nullopt;
  CallUpdateMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  const uint8_t flags = data[off];
  m.mic_muted = (flags & 0x01) != 0;
  m.camera_on = (flags & 0x02) != 0;
  m.screen_share = (flags & 0x04) != 0;
  return m;
}

ByteBuffer CallRosterMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Roster));
  write_call_id(out, call_id);
  const uint16_t n = static_cast<uint16_t>(std::min(participants.size(), kMaxRoster));
  write_u16_le(out, n);
  for (uint16_t i = 0; i < n; ++i) write_user_id(out, participants[i]);
  return out;
}

std::optional<CallRosterMessage> CallRosterMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kCallIdSize + 2 || data[0] != static_cast<uint8_t>(CallKind::Roster))
    return std::nullopt;
  CallRosterMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  const uint16_t n = read_u16_le(data.data() + off);
  off += 2;
  if (n > kMaxRoster || off + static_cast<std::size_t>(n) * kPublicKeySize > data.size())
    return std::nullopt;
  m.participants.resize(n);
  for (uint16_t i = 0; i < n; ++i) {
    if (!read_user_id(data, off, m.participants[i])) return std::nullopt;
  }
  return m;
}

ByteBuffer CallPeerIntroMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::PeerIntro));
  write_call_id(out, call_id);
  write_user_id(out, peer.user_id);
  write_str(out, peer.host);
  write_u16_le(out, peer.port);
  return out;
}

std::optional<CallPeerIntroMessage> CallPeerIntroMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kCallIdSize + kPublicKeySize + 2 + 2 ||
      data[0] != static_cast<uint8_t>(CallKind::PeerIntro))
    return std::nullopt;
  CallPeerIntroMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  if (!read_user_id(data, off, m.peer.user_id)) return std::nullopt;
  if (!read_str(data, off, kMaxHost, m.peer.host)) return std::nullopt;
  if (off + 2 > data.size()) return std::nullopt;
  m.peer.port = read_u16_le(data.data() + off);
  return m;
}

ByteBuffer CallPeerGoneMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::PeerGone));
  write_call_id(out, call_id);
  write_user_id(out, user_id);
  return out;
}

std::optional<CallPeerGoneMessage> CallPeerGoneMessage::decode(const ByteBuffer& data) {
  if (data.size() != 1 + kCallIdSize + kPublicKeySize ||
      data[0] != static_cast<uint8_t>(CallKind::PeerGone))
    return std::nullopt;
  CallPeerGoneMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  if (!read_user_id(data, off, m.user_id)) return std::nullopt;
  return m;
}

ByteBuffer CallEndpointMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(CallKind::Endpoint));
  write_call_id(out, call_id);
  write_user_id(out, self.user_id);
  write_str(out, self.host);
  write_u16_le(out, self.port);
  return out;
}

std::optional<CallEndpointMessage> CallEndpointMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kCallIdSize + kPublicKeySize + 2 + 2 ||
      data[0] != static_cast<uint8_t>(CallKind::Endpoint))
    return std::nullopt;
  CallEndpointMessage m;
  std::size_t off = 1;
  if (!read_call_id(data, off, m.call_id)) return std::nullopt;
  if (!read_user_id(data, off, m.self.user_id)) return std::nullopt;
  if (!read_str(data, off, kMaxHost, m.self.host)) return std::nullopt;
  if (off + 2 > data.size()) return std::nullopt;
  m.self.port = read_u16_le(data.data() + off);
  return m;
}

}  // namespace nyx
