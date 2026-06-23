#include "node_service.hpp"

#include "nyx/file_proto.hpp"
#include "nyx/identity.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>

namespace nyx_app {

namespace {

bool group_id_is_zero(const nyx::GroupId& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
}

nyx::GroupId group_from_hex(const std::string& hex) {
  nyx::GroupId gid{};
  nyx::GroupStore::group_id_from_hex(hex, gid);
  return gid;
}

nyx::UserId user_from_hex(const std::string& hex) {
  nyx::UserId uid{};
  nyx::ByteBuffer buf;
  if (nyx::from_hex(hex, buf) && buf.size() == uid.size()) {
    std::memcpy(uid.data(), buf.data(), buf.size());
  }
  return uid;
}

}  // namespace

std::string NodeService::resolve_share_root_path(const std::string& root_path) const {
  if (root_path.empty()) return root_path;
  const std::string norm = nyx::normalize_grant_root(root_path);
  for (const auto& r : file_index_.share_roots()) {
    if (nyx::normalize_grant_root(r.path) == norm) return r.path;
  }
  return norm;
}

void NodeService::after_file_access_changed(const std::string& scope_group_id_hex) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_hub_ && share_scope_group_ == scope) {
    group_hub_->broadcast_file_access_policy();
  }
}

bool NodeService::try_apply_file_access_policy(const nyx::ByteBuffer& payload) {
  if (payload.empty()) return false;
  if (static_cast<nyx::FileKind>(payload[0]) != nyx::FileKind::PolicyPush) return false;
  const auto policy = nyx::decode_policy_push(payload);
  if (!policy) return false;
  if (!file_access_.import_policy(*policy)) return false;

  FileAccessSyncCallback cb;
  {
    std::lock_guard lock(cb_mutex_);
    cb = on_file_access_sync_;
  }
  if (cb) cb();
  return true;
}

uint32_t NodeService::my_file_permissions(const std::string& scope_group_id_hex,
                                          const std::string& root_path,
                                          const std::string& relative_path) const {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return nyx::kFilePermissionAll;

  const auto profile = load_profile();
  nyx::GroupStore store;
  store.load();
  const auto group = store.find(scope);
  if (!group) return nyx::kFilePermissionAll;
  if (group->owner_id == profile.user_id()) return nyx::kFilePermissionAll;
  for (const auto& m : group->members) {
    if (m.role == nyx::GroupRole::Owner && m.user_id == profile.user_id()) {
      return nyx::kFilePermissionAll;
    }
  }

  file_access_.ensure_policy(scope, *group);
  const std::string root = root_path.empty() ? root_path : resolve_share_root_path(root_path);
  return file_access_.permissions_for(scope, profile.user_id(), root, relative_path);
}

nyx::GroupFileAccess NodeService::file_access_policy(const std::string& scope_group_id_hex) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  nyx::GroupStore store;
  store.load();
  const auto group = store.find(scope);
  if (!group) {
    nyx::GroupRecord stub;
    stub.id = scope;
    return nyx::FileAccessStore::default_policy(scope, stub);
  }
  return file_access_.ensure_policy(scope, *group);
}

bool NodeService::set_member_file_role(const std::string& scope_group_id_hex,
                                       const std::string& user_id_hex,
                                       const std::string& role_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  if (!file_access_.set_member_role(scope, user_from_hex(user_id_hex), role_id)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::upsert_file_role(const std::string& scope_group_id_hex,
                                   const nyx::FileRole& role) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  if (!file_access_.upsert_role(scope, role)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::remove_file_role(const std::string& scope_group_id_hex,
                                   const std::string& role_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  if (!file_access_.remove_role(scope, role_id)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::set_root_member_file_role(const std::string& scope_group_id_hex,
                                            const std::string& root_path,
                                            const std::string& user_id_hex,
                                            const std::string& role_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  const std::string root = resolve_share_root_path(root_path);
  if (!file_access_.set_root_member_role(scope, root, user_from_hex(user_id_hex), role_id)) {
    return false;
  }
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::set_path_member_file_role(const std::string& scope_group_id_hex,
                                            const std::string& root_path,
                                            const std::string& relative_path,
                                            const std::string& user_id_hex,
                                            const std::string& role_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  const std::string root = resolve_share_root_path(root_path);
  if (!file_access_.set_path_member_role(scope, root, relative_path, user_from_hex(user_id_hex),
                                       role_id)) {
    return false;
  }
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::set_path_direct_file_permissions(const std::string& scope_group_id_hex,
                                                   const std::string& root_path,
                                                   const std::string& relative_path,
                                                   const std::string& user_id_hex,
                                                   uint32_t permissions) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  const std::string root = resolve_share_root_path(root_path);
  if (!file_access_.set_path_direct_permissions(scope, root, relative_path,
                                                user_from_hex(user_id_hex), permissions)) {
    return false;
  }
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::set_path_role(const std::string& scope_group_id_hex,
                                const std::string& root_path,
                                const std::string& relative_path,
                                const std::string& role_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  const std::string root = resolve_share_root_path(root_path);
  if (!file_access_.set_path_role(scope, root, relative_path, role_id)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::upsert_permission_preset(const std::string& scope_group_id_hex,
                                           const nyx::FilePermissionPreset& preset) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  file_access_policy(scope_group_id_hex);
  if (!file_access_.upsert_permission_preset(scope, preset)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

bool NodeService::remove_permission_preset(const std::string& scope_group_id_hex,
                                           const std::string& preset_id) {
  const nyx::GroupId scope = group_from_hex(scope_group_id_hex);
  if (group_id_is_zero(scope)) return false;
  if (!file_access_.remove_permission_preset(scope, preset_id)) return false;
  after_file_access_changed(scope_group_id_hex);
  return true;
}

std::vector<nyx::ShareRoot> NodeService::all_share_roots() const {
  return file_index_.share_roots();
}

std::vector<nyx::FileEntry> NodeService::local_files_at_root(
    const std::string& share_root_path, const std::string& parent_rel) const {
  return file_index_.listing_at_root(nyx::normalize_utf8_path(share_root_path), parent_rel);
}

void NodeService::publish_field_index() {
  if (std::all_of(share_scope_group_.begin(), share_scope_group_.end(),
                  [](uint8_t b) { return b == 0; })) {
    return;
  }
  if (group_hub_) return;

  const auto entries = file_index_.entries_for_session(share_scope_group_);
  std::vector<std::string> root_paths;
  for (const auto& r : file_index_.roots_for_session(share_scope_group_)) {
    root_paths.push_back(r.path);
  }
  if (entries.empty() && root_paths.empty()) return;
  if (!files_) return;

  if (!files_->push_field_index(entries, root_paths)) {
    emit_status("не удалось опубликовать индекс поля");
  }
}

}  // namespace nyx_app
