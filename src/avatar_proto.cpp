#include "nyx/avatar_proto.hpp"

#include "nyx/util.hpp"

#include <cstring>

namespace nyx {

namespace {

constexpr std::size_t kMaxMime = 64;
constexpr std::size_t kMaxReason = 128;

bool read_str(const ByteBuffer& data, std::size_t& off, std::size_t max_len, std::string& out) {
  if (off + 2 > data.size()) return false;
  const uint16_t len = read_u16_le(data.data() + off);
  off += 2;
  if (len > max_len || off + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + off), len);
  off += len;
  return true;
}

void write_str(ByteBuffer& out, const std::string& s) {
  write_u16_le(out, static_cast<uint16_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

}  // namespace

bool is_avatar_frame(const ByteBuffer& data) {
  if (data.empty()) return false;
  const uint8_t b = data[0];
  return b >= static_cast<uint8_t>(AvatarKind::Request) &&
         b <= static_cast<uint8_t>(AvatarKind::Deny);
}

ByteBuffer AvatarRequest::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(AvatarKind::Request));
  out.insert(out.end(), hash.begin(), hash.end());
  return out;
}

std::optional<AvatarRequest> AvatarRequest::decode(const ByteBuffer& data) {
  if (data.size() != 1 + 32 || data[0] != static_cast<uint8_t>(AvatarKind::Request))
    return std::nullopt;
  AvatarRequest m;
  std::memcpy(m.hash.data(), data.data() + 1, 32);
  return m;
}

ByteBuffer AvatarOffer::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(AvatarKind::Offer));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u64_le(out, size);
  write_str(out, mime);
  return out;
}

std::optional<AvatarOffer> AvatarOffer::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 32 + 8 + 2 || data[0] != static_cast<uint8_t>(AvatarKind::Offer))
    return std::nullopt;
  AvatarOffer m;
  std::size_t off = 1;
  std::memcpy(m.hash.data(), data.data() + off, 32);
  off += 32;
  m.size = read_u64_le(data.data() + off);
  off += 8;
  if (!read_str(data, off, kMaxMime, m.mime)) return std::nullopt;
  return m;
}

ByteBuffer AvatarChunk::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(AvatarKind::Chunk));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u32_le(out, index);
  write_u16_le(out, static_cast<uint16_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::optional<AvatarChunk> AvatarChunk::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 32 + 4 + 2 || data[0] != static_cast<uint8_t>(AvatarKind::Chunk))
    return std::nullopt;
  AvatarChunk m;
  std::size_t off = 1;
  std::memcpy(m.hash.data(), data.data() + off, 32);
  off += 32;
  m.index = read_u32_le(data.data() + off);
  off += 4;
  const uint16_t len = read_u16_le(data.data() + off);
  off += 2;
  if (off + len > data.size() || len > kAvatarChunkSize) return std::nullopt;
  m.data.assign(data.begin() + static_cast<std::ptrdiff_t>(off),
                data.begin() + static_cast<std::ptrdiff_t>(off + len));
  return m;
}

ByteBuffer AvatarDone::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(AvatarKind::Done));
  out.insert(out.end(), hash.begin(), hash.end());
  return out;
}

std::optional<AvatarDone> AvatarDone::decode(const ByteBuffer& data) {
  if (data.size() != 1 + 32 || data[0] != static_cast<uint8_t>(AvatarKind::Done))
    return std::nullopt;
  AvatarDone m;
  std::memcpy(m.hash.data(), data.data() + 1, 32);
  return m;
}

ByteBuffer AvatarDeny::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(AvatarKind::Deny));
  out.insert(out.end(), hash.begin(), hash.end());
  write_str(out, reason);
  return out;
}

std::optional<AvatarDeny> AvatarDeny::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 32 + 2 || data[0] != static_cast<uint8_t>(AvatarKind::Deny))
    return std::nullopt;
  AvatarDeny m;
  std::size_t off = 1;
  std::memcpy(m.hash.data(), data.data() + off, 32);
  off += 32;
  if (!read_str(data, off, kMaxReason, m.reason)) return std::nullopt;
  return m;
}

}  // namespace nyx
