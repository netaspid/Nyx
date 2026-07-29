#pragma once

/** @file group.hpp
 *  Fields (groups): metadata, roster, on-disk storage.
 */

#include "nyx/chat_id.hpp"
#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

enum class GroupRole : uint8_t {
  Owner = 1,
  Member = 2,
  Host = 3,
};

inline bool can_start_field_call(GroupRole role) {
  return role == GroupRole::Owner || role == GroupRole::Host ||
         role == GroupRole::Member;
}

/** Field member in the local roster. */
struct GroupMemberRecord {
  UserId user_id{};
  std::string nickname;
  GroupRole role = GroupRole::Member;
};

/** Field mode: currently invite+hub only; PublicListed reserved for rendezvous search. */
enum class GroupVisibility : uint8_t {
  Circle = 0,       /**< Private circle: invite based, the owner hosts the room. */
  PublicListed = 1, /**< Future: public, discoverable via rendezvous (local flag for now). */
};

/** Field description on disk. */
struct GroupRecord {
  GroupId id{};
  std::string name;
  UserId owner_id{};
  InviteToken invite_token{};
  std::vector<GroupMemberRecord> members;
  uint64_t created_ms = 0;
  std::string description;
  std::string direction;
  std::string tags;
  GroupVisibility visibility = GroupVisibility::Circle;
};

/** Local field storage: data_dir()/groups.json. */
class GroupStore {
 public:
  GroupStore();

  bool load();
  bool save() const;

  /** Creates a field with the owner in the roster. */
  GroupRecord create(const std::string& name, const UserId& owner_id,
                     const std::string& owner_nickname);

  /** Updates the meta of an existing field (description, tags, ...). */
  bool update_meta(const GroupId& id, const std::string& description,
                   const std::string& direction, const std::string& tags,
                   GroupVisibility visibility);

  std::optional<GroupRecord> find(const GroupId& id) const;
  std::optional<GroupRecord> find_by_invite(const InviteToken& token) const;
  bool upsert(const GroupRecord& group);
  bool remove(const GroupId& id);
  bool remove_member(const GroupId& id, const UserId& user_id);

  const std::vector<GroupRecord>& all() const { return groups_; }

  static std::string store_path();
  static GroupId generate_id();
  static InviteToken generate_invite();
  static std::string group_id_hex(const GroupId& id);
  static bool group_id_from_hex(const std::string& hex, GroupId& out);
  static std::string invite_hex(const InviteToken& token);
  static bool invite_from_hex(const std::string& hex, InviteToken& out);

  /** Adds live members into target without dropping already stored ones. */
  static void merge_member_roster(std::vector<GroupMemberRecord>& target,
                                  const std::vector<GroupMemberRecord>& live);

  /** Ensures the creator is present in members by owner_id. */
  static void ensure_roster(GroupRecord& group, const std::string& owner_nickname_fallback = {});

 private:
  std::vector<GroupRecord> groups_;
};

}  // namespace nyx
