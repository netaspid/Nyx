#pragma once

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

struct ShareRoot {
  std::string path;
  GroupId group_id {};

  bool is_personal() const {
    return std::all_of(group_id.begin(), group_id.end(), [](uint8_t b) { return b == 0; });
  }
};

struct FileEntry {
  FileHash hash {};
  uint64_t size = 0;
  uint64_t mtime_ms = 0;
  std::string root_path;
  std::string relative_path;
  std::string mime;
  GroupId share_group {};

  UserId owner_id {};

  std::string absolute_path() const;
  std::string display_name() const { return relative_path; }

  std::string leaf_name() const;

  bool is_directory() const { return mime == "application/x-nyx-directory"; }
};

class FileIndex {
public:

  using ScanProgressFn =
      std::function<void(const std::string& path, int files_scanned, bool finished)>;

  FileIndex();


  void clear();


  bool add_root(const std::string& root_path,
                const GroupId* group_id = nullptr,
                ScanProgressFn progress = nullptr);


  bool remove_root(const std::string& root_path, const GroupId* group_id = nullptr);


  std::vector<ShareRoot> roots_for_session(const GroupId& session_group) const;

  std::vector<ShareRoot> share_roots() const;

  std::vector<FileEntry> entries() const;


  std::vector<FileEntry> entries_for_session(const GroupId& session_group) const;


  std::vector<FileEntry> listing_for_session(const GroupId& session_group) const;


  static std::vector<FileEntry> listing_level(const std::vector<FileEntry>& source,
                                              const std::string& share_root_path,
                                              const std::string& parent_rel);

  std::vector<FileEntry> listing_level_for_root(const GroupId& session_group,
                                                const std::string& share_root_path,
                                                const std::string& parent_rel) const;


  std::vector<FileEntry> listing_at_root(const std::string& share_root_path,
                                         const std::string& parent_rel,
                                         const GroupId* scope_group = nullptr) const;


  int count_in_root(const std::string& root_path) const;
  int count_in_root(const std::string& root_path, const GroupId& scope_group) const;


  bool rescan_root(const std::string& root_path,
                   const GroupId* group_id = nullptr,
                   ScanProgressFn progress = nullptr);

  static FileEntry make_directory_marker(const ShareRoot& root,
                                         int file_count,
                                         const std::string& label_prefix = {});

  std::optional<FileEntry> find_by_hash(const FileHash& hash) const;
  std::optional<FileEntry> find_by_hash_hex(const std::string& hex) const;

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


  static std::string library_root_path(const GroupId& scope_group);

  static std::string library_owner_dir(const GroupId& scope_group, const UserId& owner_id);

  bool ensure_library_root(const GroupId& scope_group);


  std::optional<FileEntry> find_for_session(const FileHash& hash,
                                            const GroupId& session_group) const;

  static std::string guess_mime(const std::string& path);
  static std::string group_id_hex(const GroupId& id);

  bool load();
  bool save() const;


  bool reload() { return load(); }

  static std::string index_path();

private:
  bool scan_directory(const ShareRoot& root, ScanProgressFn progress = nullptr);

  mutable std::recursive_mutex mutex_;
  std::vector<ShareRoot> share_roots_;
  std::vector<FileEntry> entries_;
};

}
