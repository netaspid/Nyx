#pragma once

/** @file group_proto.hpp
 *  Group protocol frames on kChatStream.
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
  /**
   * Field meta from the hub to members.
   * Must not be 4: byte 4 = ChatKind::Bye, so Meta would parse as Bye and kill the session.
   */
  Meta = 0x40,
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

struct GroupMetaMessage {
  std::string description;
  std::string direction;
  std::string tags;
  GroupVisibility visibility = GroupVisibility::Circle;

  ByteBuffer encode() const;
  static std::optional<GroupMetaMessage> decode(const ByteBuffer& data);
};

/** Detects a GroupKind by the first byte (as opposed to ChatKind). */
bool is_group_frame(const ByteBuffer& data);

}  // namespace nyx
