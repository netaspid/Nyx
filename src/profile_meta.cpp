#include "nyx/profile_meta.hpp"

#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

namespace {

uint64_t wall_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

namespace nyx {

namespace {

constexpr std::size_t kMaxBioLen = 280;
constexpr std::size_t kMaxInterestsLen = 200;

std::string meta_path() { return data_dir() + "/profile_meta.json"; }

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
    if (c == '\\' && i < json.size())
      out.push_back(json[i++]);
    else
      out.push_back(c);
  }
  return out;
}

void clamp_meta(ProfileMeta& m) {
  if (m.bio.size() > kMaxBioLen) m.bio.resize(kMaxBioLen);
  if (m.interests.size() > kMaxInterestsLen) m.interests.resize(kMaxInterestsLen);
}

}  // namespace

std::string availability_to_string(Availability a) {
  switch (a) {
    case Availability::Away:
      return "away";
    case Availability::Busy:
      return "busy";
    case Availability::Invisible:
      return "invisible";
    default:
      return "available";
  }
}

Availability availability_from_string(const std::string& s) {
  if (s == "away") return Availability::Away;
  if (s == "busy") return Availability::Busy;
  if (s == "invisible") return Availability::Invisible;
  return Availability::Available;
}

std::string availability_label_ru(Availability a) {
  switch (a) {
    case Availability::Away:
      return "отошёл";
    case Availability::Busy:
      return "занят";
    case Availability::Invisible:
      return "невидимый";
    default:
      return "доступен";
  }
}

bool load_profile_meta(ProfileMeta& out) {
  out = ProfileMeta{};
  std::ifstream file(meta_path(), std::ios::binary);
  if (!file) return true;
  std::ostringstream ss;
  ss << file.rdbuf();
  const std::string json = ss.str();
  if (auto bio = json_get_string(json, "bio")) out.bio = *bio;
  if (auto interests = json_get_string(json, "interests")) out.interests = *interests;
  if (auto av = json_get_string(json, "availability")) {
    out.availability = availability_from_string(*av);
  }
  const auto key = json.find("\"updated_ms\":");
  if (key != std::string::npos) {
    try {
      out.updated_ms = std::stoull(json.substr(key + 13));
    } catch (const std::exception&) {
      out.updated_ms = 0;
    }
  }
  clamp_meta(out);
  return true;
}

bool save_profile_meta(const ProfileMeta& meta) {
  ProfileMeta m = meta;
  clamp_meta(m);
  if (m.updated_ms == 0) m.updated_ms = wall_ms();
  ensure_data_dir();
  std::ofstream file(meta_path(), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "{\"v\":1,\"bio\":\"" << json_escape(m.bio) << "\",\"interests\":\""
       << json_escape(m.interests) << "\",\"availability\":\""
       << availability_to_string(m.availability) << "\",\"updated_ms\":" << m.updated_ms
       << "}\n";
  return static_cast<bool>(file);
}

void append_profile_meta_wire(ByteBuffer& out, const ProfileMeta& meta) {
  ProfileMeta m = meta;
  clamp_meta(m);
  write_u16_le(out, static_cast<uint16_t>(m.bio.size()));
  out.insert(out.end(), m.bio.begin(), m.bio.end());
  write_u16_le(out, static_cast<uint16_t>(m.interests.size()));
  out.insert(out.end(), m.interests.begin(), m.interests.end());
  out.push_back(static_cast<uint8_t>(m.availability));
  write_u64_le(out, m.updated_ms);
  const uint8_t count =
      static_cast<uint8_t>(std::min(m.photo_hashes.size(), kMaxProfilePhotosWire));
  out.push_back(count);
  for (uint8_t i = 0; i < count; ++i) {
    out.insert(out.end(), m.photo_hashes[i].begin(), m.photo_hashes[i].end());
  }
}

bool read_profile_meta_wire(const ByteBuffer& data, std::size_t& offset, ProfileMeta& out) {
  if (offset + 2 > data.size()) return false;
  const uint16_t bio_len = read_u16_le(data.data() + offset);
  offset += 2;
  if (bio_len > kMaxBioLen || offset + bio_len > data.size()) return false;
  out.bio.assign(reinterpret_cast<const char*>(data.data() + offset), bio_len);
  offset += bio_len;
  if (offset + 2 > data.size()) return false;
  const uint16_t int_len = read_u16_le(data.data() + offset);
  offset += 2;
  if (int_len > kMaxInterestsLen || offset + int_len > data.size()) return false;
  out.interests.assign(reinterpret_cast<const char*>(data.data() + offset), int_len);
  offset += int_len;
  if (offset >= data.size()) return false;
  out.availability = static_cast<Availability>(data[offset]);
  ++offset;
  out.photo_hashes.clear();
  out.updated_ms = wall_ms();
  // Расширение: updated_ms + photo hashes (старые пиры не шлют).
  if (offset + 8 <= data.size()) {
    out.updated_ms = read_u64_le(data.data() + offset);
    offset += 8;
    if (offset < data.size()) {
      const uint8_t count = data[offset++];
      if (count > kMaxProfilePhotosWire) return false;
      for (uint8_t i = 0; i < count; ++i) {
        if (offset + 32 > data.size()) return false;
        FileHash h{};
        std::memcpy(h.data(), data.data() + offset, 32);
        offset += 32;
        out.photo_hashes.push_back(h);
      }
    }
  }
  return true;
}

}  // namespace nyx
