#include "nyx/file_index.hpp"

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

namespace nyx {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\')
      out += "\\\\";
    else if (c == '"')
      out += "\\\"";
    else
      out += c;
  }
  return out;
}

std::optional<std::string> json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"') break;
    if (c == '\\' && i < json.size()) out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

uint64_t json_get_u64(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) return 0;
  try {
    return std::stoull(json.substr(pos + needle.size()));
  } catch (const std::exception&) {
    return 0;
  }
}

bool group_id_is_zero(const GroupId& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
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
  entries_.clear();
  share_roots_.clear();
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

  std::map<std::string, int> subdirs;
  std::vector<FileEntry> files;
  GroupId share_group{};
  bool share_group_set = false;

  for (const auto& e : source) {
    if (e.is_directory()) continue;
    if (normalize_utf8_path(e.root_path) != root_norm) continue;
    if (!share_group_set &&
        !std::all_of(e.share_group.begin(), e.share_group.end(), [](uint8_t b) { return b == 0; })) {
      share_group = e.share_group;
      share_group_set = true;
    }

    const std::string rel = path_to_posix(e.relative_path);
    std::string rest;
    if (parent.empty()) {
      rest = rel;
    } else {
      if (rel.size() <= parent.size() + 1) continue;
      if (rel.compare(0, parent.size(), parent) != 0) continue;
      if (rel[parent.size()] != '/') continue;
      rest = rel.substr(parent.size() + 1);
    }
    if (rest.empty()) continue;

    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
      FileEntry file = e;
      file.relative_path = rest;
      files.push_back(std::move(file));
    } else {
      subdirs[rest.substr(0, slash)]++;
    }
  }

  std::vector<FileEntry> out;
  out.reserve(subdirs.size() + files.size());
  for (const auto& [sub, count] : subdirs) {
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
                                                  const std::string& parent_rel) const {
  const std::string norm = normalize_utf8_path(share_root_path);
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

bool FileIndex::group_id_from_hex(const std::string& hex, GroupId& out) {
  return GroupStore::group_id_from_hex(hex, out);
}

std::string FileIndex::guess_mime(const std::string& path) {
  const auto dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string ext = path.substr(dot + 1);
  if (ext == "txt" || ext == "md") return "text/plain";
  if (ext == "json") return "application/json";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "zip") return "application/zip";
  if (ext == "pdf") return "application/pdf";
  return "application/octet-stream";
}

bool FileIndex::scan_directory(const ShareRoot& root, ScanProgressFn progress) {
  std::error_code ec;
  const std::string norm = normalize_utf8_path(root.path);
  const std::filesystem::path root_fs = path_from_utf8(norm);
  if (!std::filesystem::exists(root_fs, ec) || !std::filesystem::is_directory(root_fs, ec)) {
    return false;
  }

  std::set<std::string> seen_hashes;
  int scanned = 0;
  for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(root_fs, ec)) {
    if (ec) break;
    if (!dir_entry.is_regular_file(ec)) continue;

    const std::string abs = path_to_utf8(dir_entry.path().lexically_normal());
    FileEntry entry;
    entry.root_path = norm;
    entry.share_group = root.group_id;
    const auto rel = std::filesystem::relative(dir_entry.path(), root_fs, ec);
    if (ec) continue;
    entry.relative_path = path_to_utf8(rel.lexically_normal());

    if (!hash_file(abs, entry.hash)) continue;
    const std::string hex = hash_hex(entry.hash);
    if (seen_hashes.count(hex)) continue;
    seen_hashes.insert(hex);

    entry.size = static_cast<uint64_t>(dir_entry.file_size(ec));
    const auto ftime = dir_entry.last_write_time(ec);
    entry.mtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch())
            .count());
    entry.mime = guess_mime(abs);
    entries_.push_back(std::move(entry));
    ++scanned;
    if (progress) progress(entry.relative_path, scanned, false);
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

  bool found = false;
  for (auto& existing : share_roots_) {
    if (existing.path == norm && existing.group_id == sr.group_id) {
      found = true;
      break;
    }
  }
  if (!found) share_roots_.push_back(sr);

  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) { return e.root_path == norm; }),
                  entries_.end());
  return scan_directory(sr, std::move(progress)) && save();
}

bool FileIndex::remove_root(const std::string& root_path, const GroupId* group_id) {
  const std::string norm = normalize_utf8_path(root_path);
  GroupId gid{};
  if (group_id) gid = *group_id;

  const auto root_it = std::remove_if(share_roots_.begin(), share_roots_.end(),
                                      [&](const ShareRoot& r) {
                                        return r.path == norm && r.group_id == gid;
                                      });
  if (root_it == share_roots_.end()) return false;
  share_roots_.erase(root_it, share_roots_.end());
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) { return e.root_path == norm; }),
                  entries_.end());
  return save();
}

std::vector<ShareRoot> FileIndex::roots_for_session(const GroupId& session_group) const {
  std::vector<ShareRoot> out;
  for (const auto& r : share_roots_) {
    FileEntry fake;
    fake.share_group = r.group_id;
    if (entry_visible_in_session(fake, session_group)) out.push_back(r);
  }
  return out;
}

std::vector<FileEntry> FileIndex::entries_for_session(const GroupId& session_group) const {
  std::vector<FileEntry> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) {
    if (entry_visible_in_session(e, session_group)) out.push_back(e);
  }
  return out;
}

int FileIndex::count_in_root(const std::string& root_path) const {
  const std::string norm = normalize_utf8_path(root_path);
  int count = 0;
  for (const auto& e : entries_) {
    if (e.is_directory()) continue;
    if (normalize_utf8_path(e.root_path) == norm) ++count;
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
  const std::string key = "nyx-dir:" + root.path;
  marker.hash = hash_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  return marker;
}

std::vector<FileEntry> FileIndex::listing_for_session(const GroupId& session_group) const {
  std::vector<FileEntry> out = entries_for_session(session_group);
  for (const auto& root : roots_for_session(session_group)) {
    out.insert(out.begin(), make_directory_marker(root, count_in_root(root.path)));
  }
  return out;
}

bool FileIndex::rescan_root(const std::string& root_path, const GroupId* group_id,
                            ScanProgressFn progress) {
  const std::string norm = normalize_utf8_path(root_path);
  GroupId gid{};
  if (group_id) gid = *group_id;

  ShareRoot* found = nullptr;
  for (auto& existing : share_roots_) {
    if (existing.path == norm && existing.group_id == gid) {
      found = &existing;
      break;
    }
  }
  if (!found) return false;

  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& e) { return e.root_path == norm; }),
                  entries_.end());
  return scan_directory(*found, std::move(progress)) && save();
}

std::optional<FileEntry> FileIndex::find_for_session(const FileHash& hash,
                                                     const GroupId& session_group) const {
  auto entry = find_by_hash(hash);
  if (!entry || entry->is_directory()) return std::nullopt;
  if (!entry_visible_in_session(*entry, session_group)) return std::nullopt;
  return entry;
}

bool FileIndex::load() {
  entries_.clear();
  share_roots_.clear();
  std::ifstream file(path_from_utf8(index_path()), std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  if (json.empty()) return true;

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
          group_id_from_hex(*gid, sr.group_id);
        }
        if (!sr.path.empty()) share_roots_.push_back(std::move(sr));
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
        if (auto gid = json_get_string(obj, "group")) {
          group_id_from_hex(*gid, entry.share_group);
        } else {
          for (const auto& sr : share_roots_) {
            if (sr.path == entry.root_path) {
              entry.share_group = sr.group_id;
              break;
            }
          }
        }
        entries_.push_back(std::move(entry));
      }
    }
  }
  return true;
}

bool FileIndex::save() const {
  ensure_data_dir();
  std::ofstream file(path_from_utf8(index_path()), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"roots\":[";
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
         << json_escape(e.mime) << "\",\"group\":\"" << group_id_hex(e.share_group) << "\"}";
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

std::optional<FileEntry> FileIndex::find_by_hash(const FileHash& hash) const {
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

}  // namespace nyx
