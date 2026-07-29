#include "nyx/file_index.hpp"

#include "json_text.hpp"

#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace nyx {

namespace {

bool group_id_is_zero(const GroupId& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
}

bool user_id_is_zero(const UserId& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
}

bool owner_id_from_hex(const std::string& hex, UserId& out) {
  std::vector<uint8_t> bytes;
  if (!from_hex(hex, bytes) || bytes.size() != out.size()) return false;
  std::memcpy(out.data(), bytes.data(), out.size());
  return true;
}

/** First path segment is owner hex when it looks like a 64-char user id. */
bool infer_owner_from_rel(const std::string& relative_path, UserId& out) {
  std::string posix = relative_path;
  for (char& c : posix) {
    if (c == '\\') c = '/';
  }
  const auto slash = posix.find('/');
  // Owner dirs always contain at least one file under them.
  if (slash == std::string::npos) return false;
  const std::string head = posix.substr(0, slash);
  if (head.size() != 64) return false;
  return owner_id_from_hex(head, out);
}

bool entry_visible_in_session(const FileEntry& entry, const GroupId& session_group) {
  const bool session_is_group = !group_id_is_zero(session_group);
  const bool entry_is_group = !group_id_is_zero(entry.share_group);
  if (session_is_group) {
    return entry_is_group && entry.share_group == session_group;
  }
  return !entry_is_group;
}

std::string path_to_posix(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

std::vector<std::string> split_objects(const std::string& arr) {
  std::vector<std::string> out;
  std::size_t depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const char c = arr[i];
    if (c == '{') {
      if (depth++ == 0) start = i;
    } else if (c == '}') {
      if (--depth == 0 && start != std::string::npos) {
        out.push_back(arr.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return out;
}

}  // namespace

void FileIndex::clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  share_roots_.clear();
}

std::vector<ShareRoot> FileIndex::share_roots() const {
  std::lock_guard lock(mutex_);
  return share_roots_;
}

std::vector<FileEntry> FileIndex::entries() const {
  std::lock_guard lock(mutex_);
  return entries_;
}

std::string FileEntry::absolute_path() const {
  return path_to_utf8((path_from_utf8(root_path) / path_from_utf8(relative_path)).lexically_normal());
}

std::string FileEntry::leaf_name() const {
  const std::string posix = path_to_posix(relative_path);
  const auto slash = posix.rfind('/');
  if (slash == std::string::npos) return posix;
  return posix.substr(slash + 1);
}

std::vector<FileEntry> FileIndex::listing_level(const std::vector<FileEntry>& source,
                                                const std::string& share_root_path,
                                                const std::string& parent_rel) {
  const std::string root_norm = normalize_utf8_path(share_root_path);
  const std::string parent = path_to_posix(parent_rel);
  const std::string root_leaf = path_to_posix(path_to_utf8(path_from_utf8(root_norm).filename()));

  std::map<std::string, int> subdirs;
  std::map<std::string, FileEntry> dir_markers;
  std::vector<FileEntry> files;
  GroupId share_group{};
  bool share_group_set = false;

  auto take_share_group = [&](const FileEntry& e) {
    if (share_group_set) return;
    if (!std::all_of(e.share_group.begin(), e.share_group.end(), [](uint8_t b) { return b == 0; })) {
      share_group = e.share_group;
      share_group_set = true;
    }
  };

  // Direct child of parent within relative_path; empty when it does not match.
  // Accepts a leaf name without the parent prefix (as in an already-trimmed ListResp).
  auto immediate_child = [&](const std::string& rel) -> std::string {
    std::string rest;
    if (parent.empty()) {
      rest = rel;
    } else if (rel.size() > parent.size() + 1 && rel.compare(0, parent.size(), parent) == 0 &&
               rel[parent.size()] == '/') {
      rest = rel.substr(parent.size() + 1);
    } else if (rel.find('/') == std::string::npos) {
      rest = rel;
    } else {
      return {};
    }
    if (rest.empty()) return {};
    const auto slash = rest.find('/');
    if (slash != std::string::npos) return {};
    return rest;
  };

  for (const auto& e : source) {
    if (normalize_utf8_path(e.root_path) != root_norm) continue;
    take_share_group(e);

    if (e.is_directory()) {
      const std::string rel = path_to_posix(e.relative_path);
      // Share-root marker (folder name / member label), not a subfolder inside the root.
      if (parent.empty()) {
        if (rel == root_leaf) continue;
        if (rel.rfind("участник:", 0) == 0) continue;
      }
      const std::string child = immediate_child(rel);
      if (child.empty()) continue;
      dir_markers[child] = e;
      const int count = static_cast<int>(e.size);
      if (subdirs[child] < count) subdirs[child] = count;
      continue;
    }

    const std::string rel = path_to_posix(e.relative_path);
    std::string rest;
    if (parent.empty()) {
      rest = rel;
    } else if (rel.size() > parent.size() + 1 && rel.compare(0, parent.size(), parent) == 0 &&
               rel[parent.size()] == '/') {
      rest = rel.substr(parent.size() + 1);
    } else if (rel.find('/') == std::string::npos) {
      // Level response already uses leaf paths: the file lives in the current parent.
      rest = rel;
    } else {
      continue;
    }
    if (rest.empty()) continue;

    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
      FileEntry file = e;
      // Full path from the share root (not a leaf): otherwise a repeated
      // listing_level with parent="node_modules" drops ".modules.yaml".
      file.relative_path = parent.empty() ? rest : parent + "/" + rest;
      files.push_back(std::move(file));
    } else {
      const std::string child = rest.substr(0, slash);
      subdirs[child]++;
    }
  }

  std::vector<FileEntry> out;
  out.reserve(subdirs.size() + files.size());
  for (const auto& [sub, count] : subdirs) {
    const auto mit = dir_markers.find(sub);
    if (mit != dir_markers.end()) {
      FileEntry marker = mit->second;
      marker.relative_path = parent.empty() ? sub : parent + "/" + sub;
      marker.root_path = root_norm;
      if (marker.size < static_cast<uint64_t>(count)) {
        marker.size = static_cast<uint64_t>(count);
      }
      out.push_back(std::move(marker));
      continue;
    }
    FileEntry marker;
    marker.root_path = root_norm;
    marker.share_group = share_group;
    marker.relative_path = parent.empty() ? sub : parent + "/" + sub;
    marker.mime = "application/x-nyx-directory";
    marker.size = static_cast<uint64_t>(count >= 0 ? count : 0);
    const std::string key = "nyx-subdir:" + root_norm + ":" + marker.relative_path;
    marker.hash = hash_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    out.push_back(std::move(marker));
  }
  out.insert(out.end(), files.begin(), files.end());
  return out;
}

std::vector<FileEntry> FileIndex::listing_level_for_root(const GroupId& session_group,
                                                         const std::string& share_root_path,
                                                         const std::string& parent_rel) const {
  return listing_level(entries_for_session(session_group), share_root_path, parent_rel);
}

std::vector<FileEntry> FileIndex::listing_at_root(const std::string& share_root_path,
                                                  const std::string& parent_rel,
                                                  const GroupId* scope_group) const {
  if (scope_group) {
    return listing_level_for_root(*scope_group, share_root_path, parent_rel);
  }
  const std::string norm = normalize_utf8_path(share_root_path);
  std::lock_guard lock(mutex_);
  std::vector<FileEntry> in_root;
  in_root.reserve(entries_.size());
  for (const auto& e : entries_) {
    if (e.is_directory()) continue;
    if (normalize_utf8_path(e.root_path) == norm) in_root.push_back(e);
  }
  return listing_level(in_root, norm, parent_rel);
}

FileIndex::FileIndex() { load(); }

std::string FileIndex::index_path() { return data_dir() + "/file_index.json"; }

std::string FileIndex::group_id_hex(const GroupId& id) {
  return GroupStore::group_id_hex(id);
}

std::string FileIndex::guess_mime(const std::string& path) {
  const auto dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string ext = path.substr(dot + 1);
  if (ext == "txt" || ext == "md") return "text/plain";
  if (ext == "json") return "application/json";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "ogg" || ext == "oga") return "audio/ogg";
  if (ext == "wav") return "audio/wav";
  if (ext == "m4a") return "audio/mp4";
  if (ext == "mp4") return "video/mp4";
  if (ext == "webm") return "video/webm";
  if (ext == "mkv") return "video/x-matroska";
  if (ext == "mov") return "video/quicktime";
  if (ext == "zip") return "application/zip";
  if (ext == "pdf") return "application/pdf";
  return "application/octet-stream";
}

bool FileIndex::scan_directory(const ShareRoot& root, ScanProgressFn progress) {
  // Caller already holds mutex_; the progress callback must not re-enter FileIndex.
  std::error_code ec;
  const std::string norm = normalize_utf8_path(root.path);
  const std::filesystem::path root_fs = path_from_utf8(norm);
  if (!std::filesystem::exists(root_fs, ec) || !std::filesystem::is_directory(root_fs, ec)) {
    return false;
  }

  std::set<std::string> seen_hashes;
  int scanned = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  try {
    std::filesystem::recursive_directory_iterator it(root_fs, options, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      std::error_code file_ec;
      if (!it->is_regular_file(file_ec) || file_ec) continue;

      const std::string abs = path_to_utf8(it->path().lexically_normal());
      FileEntry entry;
      entry.root_path = norm;
      entry.share_group = root.group_id;
      const auto rel = std::filesystem::relative(it->path(), root_fs, file_ec);
      if (file_ec) continue;
      entry.relative_path = path_to_posix(path_to_utf8(rel.lexically_normal()));
      if (entry.relative_path.empty()) continue;

      if (!hash_file(abs, entry.hash)) continue;
      const std::string hex = hash_hex(entry.hash);
      if (seen_hashes.count(hex)) continue;
      seen_hashes.insert(hex);

      entry.size = static_cast<uint64_t>(it->file_size(file_ec));
      if (file_ec) entry.size = 0;
      // file_time_type::time_since_epoch() may throw on Windows; skip it.
      entry.mtime_ms = 0;
      entry.mime = guess_mime(abs);
      infer_owner_from_rel(entry.relative_path, entry.owner_id);
      const std::string rel_for_progress = entry.relative_path;
      entries_.push_back(std::move(entry));
      ++scanned;
      if (progress) progress(rel_for_progress, scanned, false);
    }
  } catch (const std::exception&) {
    // Broken path / symlink loop: return whatever was scanned so far.
  }
  if (progress) progress({}, scanned, true);
  return true;
}

bool FileIndex::add_root(const std::string& root_path, const GroupId* group_id,
                          ScanProgressFn progress) {
  std::error_code ec;
  const std::string norm = normalize_utf8_path(root_path);
  if (!std::filesystem::exists(path_from_utf8(norm), ec)) return false;

  ShareRoot sr;
  sr.path = norm;
  if (group_id) sr.group_id = *group_id;

  std::lock_guard lock(mutex_);
  bool found = false;
  for (auto& existing : share_roots_) {
    if (normalize_utf8_path(existing.path) == norm && existing.group_id == sr.group_id) {
      found = true;
      break;
    }
  }
  if (!found) share_roots_.push_back(sr);

  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) {
                                  return normalize_utf8_path(e.root_path) == norm &&
                                         e.share_group == sr.group_id;
                                }),
                  entries_.end());
  return scan_directory(sr, std::move(progress)) && save();
}

bool FileIndex::remove_root(const std::string& root_path, const GroupId* group_id) {
  const std::string norm = normalize_utf8_path(root_path);
  GroupId gid{};
  if (group_id) gid = *group_id;

  std::lock_guard lock(mutex_);
  const auto root_it = std::remove_if(share_roots_.begin(), share_roots_.end(),
                                      [&](const ShareRoot& r) {
                                        return normalize_utf8_path(r.path) == norm &&
                                               r.group_id == gid;
                                      });
  if (root_it == share_roots_.end()) return false;
  share_roots_.erase(root_it, share_roots_.end());
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) {
                                  return normalize_utf8_path(e.root_path) == norm &&
                                         e.share_group == gid;
                                }),
                  entries_.end());
  return save();
}

std::vector<ShareRoot> FileIndex::roots_for_session(const GroupId& session_group) const {
  std::lock_guard lock(mutex_);
  std::vector<ShareRoot> out;
  for (const auto& r : share_roots_) {
    FileEntry fake;
    fake.share_group = r.group_id;
    if (entry_visible_in_session(fake, session_group)) out.push_back(r);
  }
  return out;
}

std::vector<FileEntry> FileIndex::entries_for_session(const GroupId& session_group) const {
  std::lock_guard lock(mutex_);
  std::vector<FileEntry> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) {
    if (entry_visible_in_session(e, session_group)) out.push_back(e);
  }
  return out;
}

int FileIndex::count_in_root(const std::string& root_path) const {
  const std::string norm = normalize_utf8_path(root_path);
  std::lock_guard lock(mutex_);
  int count = 0;
  for (const auto& e : entries_) {
    if (e.is_directory()) continue;
    if (normalize_utf8_path(e.root_path) == norm) ++count;
  }
  return count;
}

int FileIndex::count_in_root(const std::string& root_path,
                             const GroupId& scope_group) const {
  const std::string norm = normalize_utf8_path(root_path);
  std::lock_guard lock(mutex_);
  int count = 0;
  for (const auto& e : entries_) {
    if (e.is_directory()) continue;
    if (normalize_utf8_path(e.root_path) == norm && e.share_group == scope_group) ++count;
  }
  return count;
}

FileEntry FileIndex::make_directory_marker(const ShareRoot& root, int file_count,
                                           const std::string& label_prefix) {
  FileEntry marker;
  marker.root_path = root.path;
  marker.share_group = root.group_id;
  const auto fname = path_to_utf8(path_from_utf8(root.path).filename());
  marker.relative_path = label_prefix + (fname.empty() ? root.path : fname);
  marker.mime = "application/x-nyx-directory";
  marker.size = static_cast<uint64_t>(file_count >= 0 ? file_count : 0);
  const std::string key =
      "nyx-dir:" + group_id_hex(root.group_id) + ":" + root.path;
  marker.hash = hash_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  return marker;
}

std::vector<FileEntry> FileIndex::listing_for_session(const GroupId& session_group) const {
  std::lock_guard lock(mutex_);
  std::vector<FileEntry> out;
  out.reserve(entries_.size() + share_roots_.size());
  for (const auto& e : entries_) {
    if (entry_visible_in_session(e, session_group)) out.push_back(e);
  }
  for (const auto& r : share_roots_) {
    FileEntry fake;
    fake.share_group = r.group_id;
    if (!entry_visible_in_session(fake, session_group)) continue;
    int count = 0;
    const std::string norm = normalize_utf8_path(r.path);
    for (const auto& e : entries_) {
      if (e.is_directory()) continue;
      if (normalize_utf8_path(e.root_path) == norm && e.share_group == r.group_id) ++count;
    }
    out.insert(out.begin(), make_directory_marker(r, count));
  }
  return out;
}

bool FileIndex::rescan_root(const std::string& root_path, const GroupId* group_id,
                            ScanProgressFn progress) {
  const std::string norm = normalize_utf8_path(root_path);
  GroupId gid{};
  if (group_id) gid = *group_id;

  std::lock_guard lock(mutex_);
  ShareRoot* found = nullptr;
  for (auto& existing : share_roots_) {
    if (normalize_utf8_path(existing.path) == norm && existing.group_id == gid) {
      found = &existing;
      break;
    }
  }
  if (!found) return false;

  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) {
                                  return normalize_utf8_path(e.root_path) == norm &&
                                         e.share_group == gid;
                                }),
                  entries_.end());
  return scan_directory(*found, std::move(progress)) && save();
}

std::optional<FileEntry> FileIndex::find_for_session(const FileHash& hash,
                                                     const GroupId& session_group) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : entries_) {
    if (entry.hash != hash || entry.is_directory()) continue;
    if (entry_visible_in_session(entry, session_group)) return entry;
  }
  return std::nullopt;
}

bool FileIndex::load() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  share_roots_.clear();
  std::ifstream file(path_from_utf8(index_path()), std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  if (json.empty()) return true;

  // schema_version is informational; missing field means legacy v0/v1 index.
  (void)json_get_u64(json, "schema_version");

  const auto roots_key = json.find("\"roots\"");
  if (roots_key != std::string::npos) {
    const auto arr_start = json.find('[', roots_key);
    const auto arr_end = json.find(']', arr_start);
    if (arr_start != std::string::npos && arr_end != std::string::npos) {
      for (const auto& obj : split_objects(json.substr(arr_start, arr_end - arr_start + 1))) {
        ShareRoot sr;
        if (auto root = json_get_string(obj, "root")) {
          sr.path = normalize_utf8_path(*root);
        }
        if (auto gid = json_get_string(obj, "group")) {
          GroupStore::group_id_from_hex(*gid, sr.group_id);
        }
        std::error_code ec;
        // Stale roots (missing on disk) are dropped; orphan entries follow below.
        if (!sr.path.empty() && std::filesystem::is_directory(path_from_utf8(sr.path), ec)) {
          const bool duplicate = std::any_of(
              share_roots_.begin(), share_roots_.end(), [&](const ShareRoot& existing) {
                return normalize_utf8_path(existing.path) == sr.path &&
                       existing.group_id == sr.group_id;
              });
          if (!duplicate) share_roots_.push_back(std::move(sr));
        }
      }
    }
  }

  const auto files_key = json.find("\"files\"");
  if (files_key != std::string::npos) {
    const auto arr_start = json.find('[', files_key);
    const auto arr_end = json.rfind(']');
    if (arr_start != std::string::npos && arr_end != std::string::npos && arr_end > arr_start) {
      for (const auto& obj : split_objects(json.substr(arr_start, arr_end - arr_start + 1))) {
        FileEntry entry;
        const auto hash_pos = obj.find("\"hash\":\"");
        if (hash_pos == std::string::npos) continue;
        std::size_t i = hash_pos + 8;
        const auto end = obj.find('"', i);
        if (end == std::string::npos) continue;
        if (!hash_from_hex(obj.substr(i, end - i), entry.hash)) continue;

        entry.size = json_get_u64(obj, "size");
        entry.mtime_ms = json_get_u64(obj, "mtime");
        if (auto root = json_get_string(obj, "root")) {
          entry.root_path = normalize_utf8_path(*root);
        }
        if (auto rel = json_get_string(obj, "rel")) entry.relative_path = *rel;
        if (auto mime = json_get_string(obj, "mime")) entry.mime = *mime;
        if (auto owner = json_get_string(obj, "owner")) {
          owner_id_from_hex(*owner, entry.owner_id);
        } else {
          infer_owner_from_rel(entry.relative_path, entry.owner_id);
        }
        const bool had_group =
            json_get_string(obj, "group").has_value();
        if (auto gid = json_get_string(obj, "group")) {
          GroupStore::group_id_from_hex(*gid, entry.share_group);
        } else {
          for (const auto& sr : share_roots_) {
            if (sr.path == entry.root_path) {
              entry.share_group = sr.group_id;
              break;
            }
          }
        }
        const bool has_root = std::any_of(
            share_roots_.begin(), share_roots_.end(), [&](const ShareRoot& root) {
              return normalize_utf8_path(root.path) == entry.root_path &&
                     root.group_id == entry.share_group;
            });
        const std::string managed_prefix =
            normalize_utf8_path(data_dir() + "/objects") + "/";
        const bool is_managed =
            entry.root_path.rfind(managed_prefix, 0) == 0;
        std::error_code ec;
        if ((has_root || is_managed) &&
            std::filesystem::is_regular_file(
                path_from_utf8(entry.absolute_path()), ec)) {
          entries_.push_back(entry);
          if (!had_group && has_root) {
            for (const auto& root : share_roots_) {
              if (normalize_utf8_path(root.path) != entry.root_path ||
                  root.group_id == entry.share_group) {
                continue;
              }
              FileEntry migrated = entry;
              migrated.share_group = root.group_id;
              entries_.push_back(std::move(migrated));
            }
          }
        }
      }
    }
  }
  return save();
}

bool FileIndex::save() const {
  std::lock_guard lock(mutex_);
  ensure_data_dir();
  std::ofstream file(path_from_utf8(index_path()), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  // schema_version 2: roots keyed by (path, group); managed objects under objects/.
  file << "{\"schema_version\":2,\"roots\":[";
  for (std::size_t i = 0; i < share_roots_.size(); ++i) {
    if (i > 0) file << ',';
    const auto& r = share_roots_[i];
    file << "{\"root\":\"" << json_escape(r.path) << "\",\"group\":\""
         << group_id_hex(r.group_id) << "\"}";
  }
  file << "],\"files\":[";
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (i > 0) file << ',';
    const auto& e = entries_[i];
    file << "{\"hash\":\"" << hash_hex(e.hash) << "\",\"size\":" << e.size
         << ",\"mtime\":" << e.mtime_ms << ",\"root\":\"" << json_escape(e.root_path)
         << "\",\"rel\":\"" << json_escape(e.relative_path) << "\",\"mime\":\""
         << json_escape(e.mime) << "\",\"group\":\"" << group_id_hex(e.share_group)
         << "\"";
    if (!user_id_is_zero(e.owner_id)) {
      file << ",\"owner\":\"" << to_hex(e.owner_id.data(), e.owner_id.size()) << "\"";
    }
    file << "}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

std::optional<FileEntry> FileIndex::find_by_hash(const FileHash& hash) const {
  std::lock_guard lock(mutex_);
  for (const auto& e : entries_) {
    if (e.hash == hash) return e;
  }
  return std::nullopt;
}

std::optional<FileEntry> FileIndex::find_by_hash_hex(const std::string& hex) const {
  FileHash hash{};
  if (!hash_from_hex(hex, hash)) return std::nullopt;
  return find_by_hash(hash);
}

std::string FileIndex::library_root_path(const GroupId& scope_group) {
  // Human-readable share-root leaf for Files / Field Resources.
  const std::string leaf =
      group_id_is_zero(scope_group)
          ? std::string("Импорт")
          : (std::string("Импорт-") + group_id_hex(scope_group).substr(0, 8));
  return normalize_utf8_path(data_dir() + "/library/" + leaf);
}

std::string FileIndex::library_owner_dir(const GroupId& scope_group,
                                         const UserId& owner_id) {
  const std::string root = library_root_path(scope_group);
  if (user_id_is_zero(owner_id)) return root;
  return path_to_utf8(path_from_utf8(root) /
                      path_from_utf8(to_hex(owner_id.data(), owner_id.size())));
}

bool FileIndex::ensure_library_root(const GroupId& scope_group) {
  const std::string path = library_root_path(scope_group);
  std::error_code ec;
  std::filesystem::create_directories(path_from_utf8(path), ec);
  if (ec) return false;

  std::lock_guard lock(mutex_);
  for (const auto& existing : share_roots_) {
    if (normalize_utf8_path(existing.path) == path &&
        existing.group_id == scope_group) {
      return true;
    }
  }
  ShareRoot sr;
  sr.path = path;
  sr.group_id = scope_group;
  share_roots_.push_back(std::move(sr));
  return save();
}

std::optional<FileEntry> FileIndex::adopt_file(
    const std::string& source_path, const FileHash& expected_hash,
    const std::string& display_name, const std::string& mime,
    const GroupId& scope_group, const UserId* owner_id,
    const std::string& relative_dir) {
  FileHash actual{};
  if (!hash_file(source_path, actual) || actual != expected_hash) {
    return std::nullopt;
  }

  if (!ensure_library_root(scope_group)) return std::nullopt;

  std::string safe_name =
      path_to_utf8(path_from_utf8(display_name).filename());
  if (safe_name.empty()) safe_name = hash_hex(expected_hash);
  const std::string library_root = library_root_path(scope_group);
  UserId owner{};
  if (owner_id && !user_id_is_zero(*owner_id)) owner = *owner_id;
  std::string relative_prefix;
  std::string library_dir;
  if (!relative_dir.empty()) {
    const auto rel = path_from_utf8(relative_dir).lexically_normal();
    if (rel.is_absolute() || rel.empty() ||
        std::find(rel.begin(), rel.end(), std::filesystem::path("..")) != rel.end()) {
      return std::nullopt;
    }
    relative_prefix = path_to_utf8(rel);
    std::replace(relative_prefix.begin(), relative_prefix.end(), '\\', '/');
    if (!relative_prefix.empty() && relative_prefix.back() != '/')
      relative_prefix.push_back('/');
    library_dir = path_to_utf8(path_from_utf8(library_root) / rel);
  } else if (!user_id_is_zero(owner)) {
    relative_prefix = to_hex(owner.data(), owner.size()) + "/";
    library_dir = library_owner_dir(scope_group, owner);
  } else {
    library_dir = library_root;
  }

  // Prefer content-addressed object store, then mirror into library ShareRoot.
  const std::string object_root =
      normalize_utf8_path(data_dir() + "/objects/" + hash_hex(expected_hash));
  const std::string object_path =
      path_to_utf8(path_from_utf8(object_root) / path_from_utf8(safe_name));
  std::error_code ec;
  std::filesystem::create_directories(path_from_utf8(object_root), ec);
  if (ec) return std::nullopt;
  std::filesystem::create_directories(path_from_utf8(library_dir), ec);
  if (ec) return std::nullopt;
  const auto source_fs = path_from_utf8(source_path);
  const auto object_fs = path_from_utf8(object_path);
  if (source_fs.lexically_normal() != object_fs.lexically_normal()) {
    std::filesystem::copy_file(source_fs, object_fs,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) return std::nullopt;
  }

  std::string leaf_name = safe_name;
  std::string library_path =
      path_to_utf8(path_from_utf8(library_dir) / path_from_utf8(leaf_name));
  {
    // Avoid clobbering a different file with the same display name.
    int suffix = 1;
    while (std::filesystem::exists(path_from_utf8(library_path), ec)) {
      FileHash existing_hash{};
      if (hash_file(library_path, existing_hash) &&
          existing_hash == expected_hash) {
        break;
      }
      const auto dot = safe_name.rfind('.');
      if (dot == std::string::npos) {
        leaf_name = safe_name + "-" + std::to_string(suffix++);
      } else {
        leaf_name = safe_name.substr(0, dot) + "-" +
                    std::to_string(suffix++) + safe_name.substr(dot);
      }
      library_path = path_to_utf8(path_from_utf8(library_dir) /
                                  path_from_utf8(leaf_name));
    }
  }
  const auto library_fs = path_from_utf8(library_path);
  if (object_fs.lexically_normal() != library_fs.lexically_normal()) {
    std::filesystem::copy_file(object_fs, library_fs,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) return std::nullopt;
  }

  FileEntry entry;
  entry.hash = expected_hash;
  entry.root_path = library_root;
  entry.relative_path = relative_prefix + leaf_name;
  entry.mime = mime.empty() ? guess_mime(leaf_name) : mime;
  entry.share_group = scope_group;
  entry.owner_id = owner;
  entry.size =
      static_cast<uint64_t>(std::filesystem::file_size(library_fs, ec));
  if (ec) return std::nullopt;

  std::lock_guard lock(mutex_);
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const FileEntry& existing) {
                       if (existing.hash != expected_hash ||
                           existing.share_group != scope_group) {
                         return false;
                       }
                       return path_from_utf8(existing.relative_path).parent_path() ==
                              path_from_utf8(entry.relative_path).parent_path();
                     }),
      entries_.end());
  entries_.push_back(entry);
  if (!save()) return std::nullopt;
  return entry;
}

std::optional<FileEntry> FileIndex::import_file(
    const std::string& source_path, const std::string& display_name,
    const std::string& mime, const GroupId& scope_group,
    const UserId* owner_id, const std::string& relative_dir) {
  FileHash hash{};
  if (!hash_file(source_path, hash)) return std::nullopt;
  return adopt_file(source_path, hash, display_name, mime, scope_group,
                    owner_id, relative_dir);
}

}  // namespace nyx
