#include "nyx/avatar_store.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace nyx {

namespace {

uint64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

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

std::string ext_for_mime(const std::string& mime) {
  if (mime == "image/png") return ".png";
  return ".jpg";
}

std::string mime_for_path(const std::string& path) {
  if (path.size() >= 4) {
    const auto lower = path.substr(path.size() - 4);
    if (lower == ".png" || lower == ".PNG") return "image/png";
  }
  return "image/jpeg";
}

}  // namespace

AvatarStore::AvatarStore() = default;

std::string AvatarStore::self_dir() { return data_dir() + "/avatars/self"; }

std::string AvatarStore::peers_dir() { return data_dir() + "/avatars/peers"; }

std::string AvatarStore::store_json_path() const { return data_dir() + "/avatars/photos.json"; }

std::string AvatarStore::path_for(const FileHash& hash) const {
  const std::string hex = hash_hex(hash);
  for (const auto& e : photos_) {
    if (e.hash != hash) continue;
    return self_dir() + "/" + hex + ext_for_mime(e.mime);
  }
  const std::string jpg = self_dir() + "/" + hex + ".jpg";
  const std::string png = self_dir() + "/" + hex + ".png";
  std::error_code ec;
  if (std::filesystem::exists(jpg, ec)) return jpg;
  if (std::filesystem::exists(png, ec)) return png;
  return {};
}

std::optional<AvatarEntry> AvatarStore::current() const {
  if (photos_.empty()) return std::nullopt;
  return photos_.front();
}

bool AvatarStore::load() {
  photos_.clear();
  std::ifstream file(store_json_path(), std::ios::binary);
  if (!file) return true;
  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  std::size_t pos = 0;
  while (true) {
    const auto hpos = json.find("\"hash\":\"", pos);
    if (hpos == std::string::npos) break;
    const auto hs = hpos + 8;
    const auto he = json.find('"', hs);
    if (he == std::string::npos) break;
    AvatarEntry e;
    if (!hash_from_hex(json.substr(hs, he - hs), e.hash)) {
      pos = he + 1;
      continue;
    }
    const auto mpos = json.find("\"mime\":\"", he);
    if (mpos != std::string::npos && mpos < he + 80) {
      const auto ms = mpos + 8;
      const auto me = json.find('"', ms);
      if (me != std::string::npos) e.mime = json.substr(ms, me - ms);
    }
    const auto tpos = json.find("\"set_ms\":", he);
    if (tpos != std::string::npos && tpos < he + 120) {
      try {
        e.set_ms = std::stoull(json.substr(tpos + 9));
      } catch (...) {
        e.set_ms = 0;
      }
    }
    photos_.push_back(e);
    pos = he + 1;
    if (photos_.size() >= kMaxAvatarHistory) break;
  }
  return true;
}

bool AvatarStore::save() const {
  ensure_data_dir();
  std::error_code ec;
  std::filesystem::create_directories(self_dir(), ec);
  std::ofstream file(store_json_path(), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"photos\":[";
  for (std::size_t i = 0; i < photos_.size(); ++i) {
    if (i) file << ',';
    file << "{\"hash\":\"" << hash_hex(photos_[i].hash) << "\",\"mime\":\""
         << json_escape(photos_[i].mime) << "\",\"set_ms\":" << photos_[i].set_ms << '}';
  }
  file << "]}\n";
  return static_cast<bool>(file);
}

bool AvatarStore::set_from_file(const std::string& source_path) {
  std::ifstream in(source_path, std::ios::binary);
  if (!in) return false;
  ByteBuffer data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.empty() || data.size() > kMaxAvatarBytes) return false;

  const FileHash hash = hash_bytes(data.data(), data.size());
  const std::string mime = mime_for_path(source_path);
  ensure_data_dir();
  std::error_code ec;
  std::filesystem::create_directories(self_dir(), ec);
  const std::string dest = self_dir() + "/" + hash_hex(hash) + ext_for_mime(mime);
  {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }

  photos_.erase(std::remove_if(photos_.begin(), photos_.end(),
                               [&](const AvatarEntry& e) { return e.hash == hash; }),
                photos_.end());
  AvatarEntry e;
  e.hash = hash;
  e.mime = mime;
  e.set_ms = wall_ms();
  photos_.insert(photos_.begin(), e);
  while (photos_.size() > kMaxAvatarHistory) {
    const auto old = photos_.back();
    photos_.pop_back();
    std::filesystem::remove(path_for(old.hash), ec);
  }
  return save();
}

bool AvatarStore::make_current(const FileHash& hash) {
  auto it = std::find_if(photos_.begin(), photos_.end(),
                         [&](const AvatarEntry& e) { return e.hash == hash; });
  if (it == photos_.end()) return false;
  if (it != photos_.begin()) {
    AvatarEntry e = *it;
    photos_.erase(it);
    photos_.insert(photos_.begin(), e);
  }
  return save();
}

bool AvatarStore::remove(const FileHash& hash) {
  auto it = std::find_if(photos_.begin(), photos_.end(),
                         [&](const AvatarEntry& e) { return e.hash == hash; });
  if (it == photos_.end()) return false;
  const std::string p = path_for(hash);
  photos_.erase(it);
  std::error_code ec;
  if (!p.empty()) std::filesystem::remove(p, ec);
  return save();
}

std::string AvatarStore::peer_path(const UserId& peer, const FileHash& hash) const {
  return peers_dir() + "/" + to_hex(peer.data(), peer.size()) + "/" + hash_hex(hash) + ".jpg";
}

bool AvatarStore::has_peer_photo(const UserId& peer, const FileHash& hash) const {
  std::error_code ec;
  return std::filesystem::exists(peer_path(peer, hash), ec);
}

bool AvatarStore::cache_peer_photo(const UserId& peer, const FileHash& hash, const ByteBuffer& data,
                                   const std::string& mime) {
  if (data.empty() || data.size() > kMaxAvatarBytes) return false;
  const FileHash check = hash_bytes(data.data(), data.size());
  if (check != hash) return false;
  ensure_data_dir();
  const std::string dir = peers_dir() + "/" + to_hex(peer.data(), peer.size());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string dest = dir + "/" + hash_hex(hash) + ext_for_mime(mime);
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

bool AvatarStore::read_bytes(const FileHash& hash, ByteBuffer& out) const {
  std::string path = path_for(hash);
  if (path.empty()) {
    // поиск в peers/*
    std::error_code ec;
    const auto root = peers_dir();
    if (std::filesystem::exists(root, ec)) {
      for (const auto& ent : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (!ent.is_regular_file()) continue;
        if (ent.path().stem() == hash_hex(hash)) {
          path = ent.path().string();
          break;
        }
      }
    }
  }
  if (path.empty()) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return !out.empty();
}

}  // namespace nyx
