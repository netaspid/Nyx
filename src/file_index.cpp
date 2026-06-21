#include "nyx/file_index.hpp"

#include "nyx/group.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
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

}  // namespace

std::string FileEntry::absolute_path() const {
  std::filesystem::path p = std::filesystem::path(root_path) / relative_path;
  return p.lexically_normal().string();
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

bool FileIndex::scan_directory(const ShareRoot& root) {
  std::error_code ec;
  const std::string norm = root.path;
  if (!std::filesystem::exists(norm, ec) || !std::filesystem::is_directory(norm, ec)) {
    return false;
  }

  std::set<std::string> seen_hashes;
  for (const auto& dir_entry :
       std::filesystem::recursive_directory_iterator(norm, ec)) {
    if (ec) break;
    if (!dir_entry.is_regular_file(ec)) continue;

    const std::string abs = dir_entry.path().lexically_normal().string();
    FileEntry entry;
    entry.root_path = norm;
    entry.share_group = root.group_id;
    entry.relative_path =
        std::filesystem::relative(dir_entry.path(), entry.root_path, ec).lexically_normal().string();
    if (ec) continue;

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
  }
  return true;
}

bool FileIndex::add_root(const std::string& root_path, const GroupId* group_id) {
  std::error_code ec;
  const std::string norm = std::filesystem::path(root_path).lexically_normal().string();
  if (!std::filesystem::exists(norm, ec)) return false;

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
  return scan_directory(sr) && save();
}

std::vector<FileEntry> FileIndex::entries_for_session(const GroupId& session_group) const {
  std::vector<FileEntry> out;
  out.reserve(entries_.size());
  for (const auto& e : entries_) {
    if (entry_visible_in_session(e, session_group)) out.push_back(e);
  }
  return out;
}

std::optional<FileEntry> FileIndex::find_for_session(const FileHash& hash,
                                                     const GroupId& session_group) const {
  auto entry = find_by_hash(hash);
  if (!entry) return std::nullopt;
  if (!entry_visible_in_session(*entry, session_group)) return std::nullopt;
  return entry;
}

bool FileIndex::load() {
  entries_.clear();
  share_roots_.clear();
  std::ifstream file(index_path(), std::ios::binary);
  if (!file) return true;

  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();

  std::size_t pos = 0;
  while ((pos = json.find("\"root\":\"", pos)) != std::string::npos) {
    const auto obj_start = json.rfind('{', pos);
    const auto obj_end = json.find('}', pos);
    if (obj_start == std::string::npos || obj_end == std::string::npos) break;
    const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);

    ShareRoot sr;
    if (auto root = json_get_string(obj, "root")) sr.path = *root;
    if (auto gid = json_get_string(obj, "group")) {
      group_id_from_hex(*gid, sr.group_id);
    }
    if (!sr.path.empty()) share_roots_.push_back(std::move(sr));
    pos = obj_end;
  }

  pos = 0;
  while ((pos = json.find("\"hash\":\"", pos)) != std::string::npos) {
    FileEntry entry;
    pos += 8;
    const auto end = json.find('"', pos);
    if (end == std::string::npos) break;
    if (!hash_from_hex(json.substr(pos, end - pos), entry.hash)) {
      pos = end;
      continue;
    }
    pos = end;
    const auto obj_start = json.rfind('{', pos);
    const auto obj_end = json.find('}', pos);
    if (obj_start == std::string::npos || obj_end == std::string::npos) break;
    const std::string obj = json.substr(obj_start, obj_end - obj_start + 1);
    entry.size = json_get_u64(obj, "size");
    entry.mtime_ms = json_get_u64(obj, "mtime");
    if (auto root = json_get_string(obj, "root")) entry.root_path = *root;
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
    pos = obj_end;
  }
  return true;
}

bool FileIndex::save() const {
  ensure_data_dir();
  std::ofstream file(index_path(), std::ios::binary | std::ios::trunc);
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
