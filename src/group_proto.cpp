#include "nyx/group_proto.hpp"

#include "nyx/util.hpp"

#include <cstring>

namespace nyx {

namespace {

constexpr std::size_t kMaxNameLen = 256;
constexpr std::size_t kMaxReasonLen = 256;
constexpr std::size_t kMaxNickLen = 128;

bool read_string(const ByteBuffer& data, std::size_t offset, std::size_t len,
                 std::size_t max_len, std::string& out) {
  if (len > max_len || offset + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + offset), len);
  return true;
}

void write_member(ByteBuffer& out, const GroupMemberRecord& m) {
  out.insert(out.end(), m.user_id.begin(), m.user_id.end());
  out.push_back(static_cast<uint8_t>(m.role));
  write_u16_le(out, static_cast<uint16_t>(m.nickname.size()));
  out.insert(out.end(), m.nickname.begin(), m.nickname.end());
}

bool read_member(const ByteBuffer& data, std::size_t& off, GroupMemberRecord& m) {
  if (off + 32 + 1 + 2 > data.size()) return false;
  std::memcpy(m.user_id.data(), data.data() + off, 32);
  off += 32;
  m.role = static_cast<GroupRole>(data[off++]);
  const uint16_t nick_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, nick_len, kMaxNickLen, m.nickname)) return false;
  off += nick_len;
  return true;
}

}  // namespace

bool is_group_frame(const ByteBuffer& data) {
  if (data.empty()) return false;
  const uint8_t b = data[0];
  return b >= static_cast<uint8_t>(GroupKind::Join) &&
         b <= static_cast<uint8_t>(GroupKind::MemberJoined);
}

ByteBuffer GroupJoinMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(GroupKind::Join));
  out.insert(out.end(), group_id.begin(), group_id.end());
  return out;
}

std::optional<GroupJoinMessage> GroupJoinMessage::decode(const ByteBuffer& data) {
  if (data.size() < 33 || data[0] != static_cast<uint8_t>(GroupKind::Join)) {
    return std::nullopt;
  }
  GroupJoinMessage msg;
  std::memcpy(msg.group_id.data(), data.data() + 1, 32);
  return msg;
}

ByteBuffer GroupJoinAckMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(GroupKind::JoinAck));
  out.push_back(accepted ? 1 : 0);
  write_u16_le(out, static_cast<uint16_t>(reason.size()));
  out.insert(out.end(), reason.begin(), reason.end());
  out.insert(out.end(), group_id.begin(), group_id.end());
  write_u16_le(out, static_cast<uint16_t>(group_name.size()));
  out.insert(out.end(), group_name.begin(), group_name.end());
  write_u16_le(out, static_cast<uint16_t>(members.size()));
  for (const auto& m : members) write_member(out, m);
  return out;
}

std::optional<GroupJoinAckMessage> GroupJoinAckMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 1 + 2 + 32 + 2 + 2 || data[0] != static_cast<uint8_t>(GroupKind::JoinAck)) {
    return std::nullopt;
  }
  GroupJoinAckMessage msg;
  std::size_t off = 1;
  msg.accepted = data[off++] != 0;
  const uint16_t reason_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, reason_len, kMaxReasonLen, msg.reason)) return std::nullopt;
  off += reason_len;
  if (off + 32 + 2 > data.size()) return std::nullopt;
  std::memcpy(msg.group_id.data(), data.data() + off, 32);
  off += 32;
  const uint16_t name_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, name_len, kMaxNameLen, msg.group_name)) return std::nullopt;
  off += name_len;
  if (off + 2 > data.size()) return std::nullopt;
  const uint16_t count = read_u16_le(data.data() + off);
  off += 2;
  for (uint16_t i = 0; i < count; ++i) {
    GroupMemberRecord m;
    if (!read_member(data, off, m)) return std::nullopt;
    msg.members.push_back(std::move(m));
  }
  return msg;
}

ByteBuffer GroupMemberJoinedMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(GroupKind::MemberJoined));
  write_member(out, member);
  return out;
}

std::optional<GroupMemberJoinedMessage> GroupMemberJoinedMessage::decode(
    const ByteBuffer& data) {
  if (data.size() < 2 || data[0] != static_cast<uint8_t>(GroupKind::MemberJoined)) {
    return std::nullopt;
  }
  GroupMemberJoinedMessage msg;
  std::size_t off = 1;
  if (!read_member(data, off, msg.member)) return std::nullopt;
  return msg;
}

}  // namespace nyx
