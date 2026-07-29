#pragma once

/** @file file_index.hpp
 *  Index of files in shared folders: hash, size, share policy.
 */

#include "nyx/chat_id.hpp"
#include "nyx/file_hash.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

/** Index root with a visibility scope. group_id = 0 -> DM only. */
struct ShareRoot {
  std::string path;
  GroupId group_id {};

  bool is_personal() const {
    return std::all_of(group_id.begin(), group_id.end(), [](uint8_t b) { return b == 0; });
  }
};

/** Index entry. */
struct FileEntry {
  FileHash hash {};
  uint64_t size = 0;
  uint64_t mtime_ms = 0;
  std::string root_path;
  std::string relative_path;
  std::string mime;
  GroupId share_group {};
  /** Owner of chat-imported / captured media (zero = unknown / legacy flat). */
  UserId owner_id {};

  std::string absolute_path() const;
  std::string display_name() const { return relative_path; }
  /** Display name for lists (last path segment). */
  std::string leaf_name() const;
  /** Folder marker in a listing (not downloadable). */
  bool is_directory() const { return mime == "application/x-nyx-directory"; }
};

/** Scans and stores file metadata. */
class FileIndex {
public:
  /** progress(path, files_scanned, finished). */
  using ScanProgressFn =
      std::function<void(const std::string& path, int files_scanned, bool finished)>;

  FileIndex();

  /** Clears the in-memory index (nothing written to disk). */
  void clear();

  /** Adds a root and scans files (thread-safe). group_id nullptr/zero -> DM scope. */
  bool add_root(const std::string& root_path,
                const GroupId* group_id = nullptr,
                ScanProgressFn progress = nullptr);

  /** Removes a root and its files; re-adding the same path later is allowed. */
  bool remove_root(const std::string& root_path, const GroupId* group_id = nullptr);

  /** Roots visible in a scope (DM or field). */
  std::vector<ShareRoot> roots_for_session(const GroupId& session_group) const;
  /** Copy of the roots (safe against concurrent scans). */
  std::vector<ShareRoot> share_roots() const;
  /** Copy of the index entries. */
  std::vector<FileEntry> entries() const;

  /** Files visible in the current session (DM or one field). */
  std::vector<FileEntry> entries_for_session(const GroupId& session_group) const;

  /** Files plus folder markers (size = number of files inside). */
  std::vector<FileEntry> listing_for_session(const GroupId& session_group) const;

  /** One tree level inside a share root (parent_rel "" = the folder root). */
  static std::vector<FileEntry> listing_level(const std::vector<FileEntry>& source,
                                              const std::string& share_root_path,
                                              const std::string& parent_rel);

  std::vector<FileEntry> listing_level_for_root(const GroupId& session_group,
                                                const std::string& share_root_path,
                                                const std::string& parent_rel) const;

  /** Level listing for a share root; when scope_group is set, only that scope. */
  std::vector<FileEntry> listing_at_root(const std::string& share_root_path,
                                         const std::string& parent_rel,
                                         const GroupId* scope_group = nullptr) const;

  /** Number of indexed files under a root. */
  int count_in_root(const std::string& root_path) const;
  int count_in_root(const std::string& root_path, const GroupId& scope_group) const;

  /** Rescans an existing root. */
  bool rescan_root(const std::string& root_path,
                   const GroupId* group_id = nullptr,
                   ScanProgressFn progress = nullptr);

  static FileEntry make_directory_marker(const ShareRoot& root,
                                         int file_count,
                                         const std::string& label_prefix = {});

  std::optional<FileEntry> find_by_hash(const FileHash& hash) const;
  std::optional<FileEntry> find_by_hash_hex(const std::string& hex) const;
  /** Copies a verified object into the managed content-addressed store.
   *  When owner_id is set, file is stored under library/<scope>/<owner_hex>/. */
  std::optional<FileEntry> adopt_file(const std::string& source_path,
                                      const FileHash& expected_hash,
                                      const std::string& display_name,
                                      const std::string& mime,
                                      const GroupId& scope_group,
                                      const UserId* owner_id = nullptr,
                                      const std::string& relative_dir = {});
  std::optional<FileEntry> import_file(const std::string& source_path,
                                       const std::string& display_name,
                                       const std::string& mime,
                                       const GroupId& scope_group,
                                       const UserId* owner_id = nullptr,
                                       const std::string& relative_dir = {});

  /** App-managed library root for scope (imports + adopted downloads). */
  static std::string library_root_path(const GroupId& scope_group);
  /** Per-owner directory under the library ShareRoot (empty owner → library root). */
  static std::string library_owner_dir(const GroupId& scope_group, const UserId& owner_id);
  /** Ensures ShareRoot exists so library files appear in Field resources. */
  bool ensure_library_root(const GroupId& scope_group);

  /** find plus a share-policy check for the session. */
  std::optional<FileEntry> find_for_session(const FileHash& hash,
                                            const GroupId& session_group) const;

  static std::string guess_mime(const std::string& path);
  static std::string group_id_hex(const GroupId& id);

  bool load();
  bool save() const;

  /** Reloads the index from the active account data_dir(). */
  bool reload() { return load(); }

  static std::string index_path();

private:
  bool scan_directory(const ShareRoot& root, ScanProgressFn progress = nullptr);

  mutable std::recursive_mutex mutex_;
  std::vector<ShareRoot> share_roots_;
  std::vector<FileEntry> entries_;
};

} // namespace nyx
