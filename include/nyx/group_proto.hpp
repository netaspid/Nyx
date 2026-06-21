#pragma once

/** @file group_proto.hpp
 *  Кадры группового протокола на kChatStream (фаза 5).
 */

#include "nyx/group.hpp"
#include "nyx/types.hpp"

#include <optional>
#include <vector>

namespace nyx {

enum class GroupKind : uint8_t {
  Join = 1,
  JoinAck = 2,
  MemberJoined = 3,
};

struct GroupJoinMessage {
  GroupId group_id{};

  ByteBuffer encode() const;
  static std::optional<GroupJoinMessage> decode(const ByteBuffer& data);
};

struct GroupJoinAckMessage {
  bool accepted = false;
  std::string reason;
  GroupId group_id{};
  std::string group_name;
  std::vector<GroupMemberRecord> members;

  ByteBuffer encode() const;
  static std::optional<GroupJoinAckMessage> decode(const ByteBuffer& data);
};

struct GroupMemberJoinedMessage {
  GroupMemberRecord member;

  ByteBuffer encode() const;
  static std::optional<GroupMemberJoinedMessage> decode(const ByteBuffer& data);
};

/** Распознаёт GroupKind по первому байту (не ChatKind). */
bool is_group_frame(const ByteBuffer& data);

}  // namespace nyx
