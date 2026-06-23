#pragma once

/** @file group.hpp
 *  Поля (группы): метаданные, roster, хранение на диске (фаза 5).
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
};

/** Участник поля в локальном roster. */
struct GroupMemberRecord {
  UserId user_id{};
  std::string nickname;
  GroupRole role = GroupRole::Member;
};

/** Описание поля на диске. */
struct GroupRecord {
  GroupId id{};
  std::string name;
  UserId owner_id{};
  InviteToken invite_token{};
  std::vector<GroupMemberRecord> members;
  uint64_t created_ms = 0;
};

/** Локальное хранилище полей: data_dir()/groups.json. */
class GroupStore {
 public:
  GroupStore();

  bool load();
  bool save() const;

  /** Создаёт поле с owner в roster. */
  GroupRecord create(const std::string& name, const UserId& owner_id,
                     const std::string& owner_nickname);

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

  /** Добавляет в target участников из live (без удаления уже сохранённых). */
  static void merge_member_roster(std::vector<GroupMemberRecord>& target,
                                  const std::vector<GroupMemberRecord>& live);

  /** Гарантирует создателя в members по owner_id. */
  static void ensure_roster(GroupRecord& group, const std::string& owner_nickname_fallback = {});

 private:
  std::vector<GroupRecord> groups_;
};

}  // namespace nyx
