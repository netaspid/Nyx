#pragma once

/** @file avatar_proto.hpp
 *  Обмен фото профиля на kBulkStream (отдельные kind, не FileKind).
 */

#include "nyx/file_hash.hpp"
#include "nyx/types.hpp"

#include <optional>
#include <string>

namespace nyx {

enum class AvatarKind : uint8_t {
  Request = 0x50,
  Offer = 0x51,
  Chunk = 0x52,
  Done = 0x53,
  Deny = 0x54,
};

constexpr std::size_t kAvatarChunkSize = 8192;

struct AvatarRequest {
  FileHash hash{};
  ByteBuffer encode() const;
  static std::optional<AvatarRequest> decode(const ByteBuffer& data);
};

struct AvatarOffer {
  FileHash hash{};
  uint64_t size = 0;
  std::string mime = "image/jpeg";
  ByteBuffer encode() const;
  static std::optional<AvatarOffer> decode(const ByteBuffer& data);
};

struct AvatarChunk {
  FileHash hash{};
  uint32_t index = 0;
  ByteBuffer data;
  ByteBuffer encode() const;
  static std::optional<AvatarChunk> decode(const ByteBuffer& data);
};

struct AvatarDone {
  FileHash hash{};
  ByteBuffer encode() const;
  static std::optional<AvatarDone> decode(const ByteBuffer& data);
};

struct AvatarDeny {
  FileHash hash{};
  std::string reason;
  ByteBuffer encode() const;
  static std::optional<AvatarDeny> decode(const ByteBuffer& data);
};

bool is_avatar_frame(const ByteBuffer& data);

}  // namespace nyx
