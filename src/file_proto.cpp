#include "nyx/file_proto.hpp"

#include "nyx/file_access.hpp"
#include "nyx/util.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nyx {

namespace {

constexpr std::size_t kMaxPathLen = 4096;
constexpr std::size_t kMaxMimeLen = 128;
constexpr std::size_t kMaxReasonLen = 256;
constexpr std::size_t kMaxChunkPayload = kFileChunkSize;

bool read_string(const ByteBuffer& data, std::size_t offset, std::size_t len,
                 std::size_t max_len, std::string& out) {
  if (len > max_len || offset + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + offset), len);
  return true;
}

}  // namespace

ByteBuffer FileOffer::encode() const {
  ByteBuffer out;
  out.reserve(1 + 32 + 8 + 2 + name.size() + 2 + mime.size());
  out.push_back(static_cast<uint8_t>(FileKind::Offer));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u64_le(out, size);
  write_u16_le(out, static_cast<uint16_t>(name.size()));
  out.insert(out.end(), name.begin(), name.end());
  write_u16_le(out, static_cast<uint16_t>(mime.size()));
  out.insert(out.end(), mime.begin(), mime.end());
  return out;
}

std::optional<FileOffer> FileOffer::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 32 + 8 + 2 + 2 || data[0] != static_cast<uint8_t>(FileKind::Offer)) {
    return std::nullopt;
  }
  FileOffer offer;
  std::memcpy(offer.hash.data(), data.data() + 1, 32);
  offer.size = read_u64_le(data.data() + 33);
  std::size_t off = 41;
  const uint16_t name_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, name_len, kMaxPathLen, offer.name)) return std::nullopt;
  off += name_len;
  if (off + 2 > data.size()) return std::nullopt;
  const uint16_t mime_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, mime_len, kMaxMimeLen, offer.mime)) return std::nullopt;
  return offer;
}

ByteBuffer FileRequest::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::Request));
  out.insert(out.end(), hash.begin(), hash.end());
  return out;
}

std::optional<FileRequest> FileRequest::decode(const ByteBuffer& data) {
  if (data.size() < 33 || data[0] != static_cast<uint8_t>(FileKind::Request)) {
    return std::nullopt;
  }
  FileRequest req;
  std::memcpy(req.hash.data(), data.data() + 1, 32);
  return req;
}

ByteBuffer FileChunk::encode() const {
  ByteBuffer out;
  out.reserve(1 + 32 + 8 + 4 + data.size());
  out.push_back(static_cast<uint8_t>(FileKind::Chunk));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u64_le(out, offset);
  write_u32_le(out, static_cast<uint32_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

std::optional<FileChunk> FileChunk::decode(const ByteBuffer& data) {
  if (data.size() < 1 + 32 + 8 + 4 || data[0] != static_cast<uint8_t>(FileKind::Chunk)) {
    return std::nullopt;
  }
  FileChunk chunk;
  std::memcpy(chunk.hash.data(), data.data() + 1, 32);
  chunk.offset = read_u64_le(data.data() + 33);
  const uint32_t len = read_u32_le(data.data() + 41);
  if (len > kMaxChunkPayload || data.size() < 45 + len) return std::nullopt;
  chunk.data.assign(data.begin() + 45, data.begin() + 45 + len);
  return chunk;
}

ByteBuffer FileComplete::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::Complete));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u64_le(out, size);
  return out;
}

std::optional<FileComplete> FileComplete::decode(const ByteBuffer& data) {
  if (data.size() < 41 || data[0] != static_cast<uint8_t>(FileKind::Complete)) {
    return std::nullopt;
  }
  FileComplete msg;
  std::memcpy(msg.hash.data(), data.data() + 1, 32);
  msg.size = read_u64_le(data.data() + 33);
  return msg;
}

ByteBuffer FileDeny::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::Deny));
  out.insert(out.end(), hash.begin(), hash.end());
  write_u16_le(out, static_cast<uint16_t>(reason.size()));
  out.insert(out.end(), reason.begin(), reason.end());
  return out;
}

std::optional<FileDeny> FileDeny::decode(const ByteBuffer& data) {
  if (data.size() < 35 || data[0] != static_cast<uint8_t>(FileKind::Deny)) return std::nullopt;
  FileDeny deny;
  std::memcpy(deny.hash.data(), data.data() + 1, 32);
  const uint16_t len = read_u16_le(data.data() + 33);
  if (!read_string(data, 35, len, kMaxReasonLen, deny.reason)) return std::nullopt;
  return deny;
}

ByteBuffer encode_list_request() {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::ListReq));
  return out;
}

ByteBuffer encode_list_request(const std::string& root_path, const std::string& parent_rel) {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::ListReq));
  write_u16_le(out, static_cast<uint16_t>(std::min<std::size_t>(root_path.size(), 65535)));
  out.insert(out.end(), root_path.begin(),
             root_path.begin() + static_cast<std::ptrdiff_t>(
                                     std::min<std::size_t>(root_path.size(), 65535)));
  write_u16_le(out, static_cast<uint16_t>(std::min<std::size_t>(parent_rel.size(), 65535)));
  out.insert(out.end(), parent_rel.begin(),
             parent_rel.begin() + static_cast<std::ptrdiff_t>(
                                      std::min<std::size_t>(parent_rel.size(), 65535)));
  return out;
}

std::optional<std::pair<std::string, std::string>> decode_list_request(const ByteBuffer& data) {
  if (data.empty() || data[0] != static_cast<uint8_t>(FileKind::ListReq)) return std::nullopt;
  if (data.size() == 1) return std::make_pair(std::string{}, std::string{});
  if (data.size() < 5) return std::nullopt;
  std::size_t off = 1;
  const uint16_t root_len = read_u16_le(data.data() + off);
  off += 2;
  std::string root;
  if (!read_string(data, off, root_len, kMaxPathLen, root)) return std::nullopt;
  off += root_len;
  if (off + 2 > data.size()) return std::nullopt;
  const uint16_t parent_len = read_u16_le(data.data() + off);
  off += 2;
  std::string parent;
  if (!read_string(data, off, parent_len, kMaxPathLen, parent)) return std::nullopt;
  return std::make_pair(std::move(root), std::move(parent));
}

ByteBuffer encode_list_response(const std::vector<FileEntry>& entries) {
  // Connection шифрует весь bulk-кадр до фрагментации; Noise ≤ 65519 байт plaintext.
  // mux добавляет 4 байта stream_id — держим запас.
  constexpr std::size_t kMaxListBytes = 48000;

  std::vector<const FileEntry*> ordered;
  ordered.reserve(entries.size());
  for (const auto& e : entries) {
    if (e.is_directory()) ordered.push_back(&e);
  }
  for (const auto& e : entries) {
    if (!e.is_directory()) ordered.push_back(&e);
  }

  ByteBuffer out;
  out.reserve(std::min(kMaxListBytes, ordered.size() * 128 + 8));
  out.push_back(static_cast<uint8_t>(FileKind::ListResp));
  write_u16_le(out, 0);  // count — заполним в конце

  uint16_t count = 0;
  for (const FileEntry* pe : ordered) {
    if (!pe || count == 65535) break;
    const auto& e = *pe;
    const auto rel_len =
        static_cast<uint16_t>(std::min(e.relative_path.size(), kMaxPathLen));
    const auto mime_len =
        static_cast<uint16_t>(std::min(e.mime.size(), kMaxMimeLen));
    const auto root_len =
        static_cast<uint16_t>(std::min(e.root_path.size(), kMaxPathLen));
    const std::size_t add =
        32 + 8 + 2 + rel_len + 2 + mime_len + 2 + root_len;
    if (out.size() + add > kMaxListBytes && count > 0) break;

    out.insert(out.end(), e.hash.begin(), e.hash.end());
    write_u64_le(out, e.size);
    write_u16_le(out, rel_len);
    out.insert(out.end(), e.relative_path.begin(), e.relative_path.begin() + rel_len);
    write_u16_le(out, mime_len);
    out.insert(out.end(), e.mime.begin(), e.mime.begin() + mime_len);
    write_u16_le(out, root_len);
    out.insert(out.end(), e.root_path.begin(), e.root_path.begin() + root_len);
    ++count;
  }

  out[1] = static_cast<uint8_t>(count & 0xff);
  out[2] = static_cast<uint8_t>((count >> 8) & 0xff);
  return out;
}

std::optional<std::vector<FileEntry>> decode_list_response(const ByteBuffer& data) {
  if (data.size() < 3 || data[0] != static_cast<uint8_t>(FileKind::ListResp)) {
    return std::nullopt;
  }
  const uint16_t count = read_u16_le(data.data() + 1);
  std::vector<FileEntry> entries;
  std::size_t off = 3;
  for (uint16_t i = 0; i < count; ++i) {
    if (off + 32 + 8 + 2 + 2 > data.size()) return std::nullopt;
    FileEntry entry;
    std::memcpy(entry.hash.data(), data.data() + off, 32);
    off += 32;
    entry.size = read_u64_le(data.data() + off);
    off += 8;
    const uint16_t path_len = read_u16_le(data.data() + off);
    off += 2;
    if (!read_string(data, off, path_len, kMaxPathLen, entry.relative_path)) {
      return std::nullopt;
    }
    off += path_len;
    const uint16_t mime_len = read_u16_le(data.data() + off);
    off += 2;
    if (!read_string(data, off, mime_len, kMaxMimeLen, entry.mime)) return std::nullopt;
    off += mime_len;
    if (off + 2 <= data.size()) {
      const uint16_t root_len = read_u16_le(data.data() + off);
      off += 2;
      if (!read_string(data, off, root_len, kMaxPathLen, entry.root_path)) return std::nullopt;
      off += root_len;
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

ByteBuffer encode_index_push(const std::vector<FileEntry>& entries,
                             const std::vector<std::string>& root_paths) {
  ByteBuffer out = encode_list_response(entries);
  if (out.empty()) return out;
  out[0] = static_cast<uint8_t>(FileKind::IndexPush);
  write_u16_le(out, static_cast<uint16_t>(root_paths.size()));
  for (const auto& path : root_paths) {
    write_u16_le(out, static_cast<uint16_t>(path.size()));
    out.insert(out.end(), path.begin(), path.end());
  }
  return out;
}

std::optional<IndexPushPayload> decode_index_push(const ByteBuffer& data) {
  if (data.size() < 3 || data[0] != static_cast<uint8_t>(FileKind::IndexPush)) {
    return std::nullopt;
  }
  ByteBuffer as_list = data;
  as_list[0] = static_cast<uint8_t>(FileKind::ListResp);
  auto entries = decode_list_response(as_list);
  if (!entries) return std::nullopt;

  IndexPushPayload payload;
  payload.entries = std::move(*entries);

  std::size_t off = 3;
  const uint16_t entry_count = read_u16_le(data.data() + 1);
  for (uint16_t i = 0; i < entry_count; ++i) {
    if (off + 32 + 8 + 2 + 2 > data.size()) break;
    off += 32 + 8;
    const uint16_t path_len = read_u16_le(data.data() + off);
    off += 2 + path_len;
    const uint16_t mime_len = read_u16_le(data.data() + off);
    off += 2 + mime_len;
    if (off + 2 <= data.size()) {
      const uint16_t root_len = read_u16_le(data.data() + off);
      off += 2 + root_len;
    }
  }
  if (off + 2 > data.size()) return payload;
  const uint16_t roots_in_payload = read_u16_le(data.data() + off);
  off += 2;
  for (uint16_t i = 0; i < roots_in_payload; ++i) {
    if (off + 2 > data.size()) break;
    const uint16_t len = read_u16_le(data.data() + off);
    off += 2;
    if (off + len > data.size()) break;
    payload.root_paths.emplace_back(reinterpret_cast<const char*>(data.data() + off), len);
    off += len;
  }
  return payload;
}

namespace {

constexpr std::size_t kMaxPolicyJson = 512 * 1024;

}  // namespace

ByteBuffer encode_policy_push(const GroupFileAccess& policy) {
  const std::string json = FileAccessStore::encode_group_policy_json(policy);
  ByteBuffer out;
  out.reserve(1 + 4 + json.size());
  out.push_back(static_cast<uint8_t>(FileKind::PolicyPush));
  write_u32_le(out, static_cast<uint32_t>(json.size()));
  out.insert(out.end(), json.begin(), json.end());
  return out;
}

std::optional<GroupFileAccess> decode_policy_push(const ByteBuffer& data) {
  if (data.size() < 5 || data[0] != static_cast<uint8_t>(FileKind::PolicyPush)) {
    return std::nullopt;
  }
  const uint32_t len = read_u32_le(data.data() + 1);
  if (len == 0 || len > kMaxPolicyJson || data.size() < 5 + len) return std::nullopt;
  const std::string json(reinterpret_cast<const char*>(data.data() + 5), len);
  GroupFileAccess policy;
  if (!FileAccessStore::decode_group_policy_json(json, policy)) return std::nullopt;
  return policy;
}

ByteBuffer encode_policy_request() {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(FileKind::PolicyReq));
  return out;
}

}  // namespace nyx
