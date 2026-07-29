#pragma once

/** @file avatar_store.hpp
 *  Local profile photos (history up to 5) and the peer avatar cache.
 */

#include "nyx/file_hash.hpp"
#include "nyx/identity.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

constexpr std::size_t kMaxAvatarHistory = 5;
constexpr std::size_t kMaxAvatarBytes = 200 * 1024;

struct AvatarEntry {
  FileHash hash{};
  std::string mime = "image/jpeg";
  uint64_t set_ms = 0;
};

/** Own avatars: current = photos[0]. */
class AvatarStore {
 public:
  AvatarStore();

  bool load();
  bool save() const;

  const std::vector<AvatarEntry>& photos() const { return photos_; }
  /** Own avatar file path by hash; empty when missing. */
  std::string path_for(const FileHash& hash) const;
  /** Current photo (first in the list). */
  std::optional<AvatarEntry> current() const;

  /**
   * Adds a JPEG/PNG from disk: copies into the store, hashes it, makes it current.
   * FIFO eviction above kMaxAvatarHistory.
   */
  bool set_from_file(const std::string& source_path);

  /** Makes an already known hash current (moves it to the front). */
  bool make_current(const FileHash& hash);

  bool remove(const FileHash& hash);

  /** Peer cache: store a blob. */
  bool cache_peer_photo(const UserId& peer, const FileHash& hash, const ByteBuffer& data,
                        const std::string& mime = "image/jpeg");
  /** Peer cache path; empty when the file is missing. */
  std::string peer_path(const UserId& peer, const FileHash& hash) const;
  bool has_peer_photo(const UserId& peer, const FileHash& hash) const;

  /** Reads own or peer file bytes by hash (self takes priority). */
  bool read_bytes(const FileHash& hash, ByteBuffer& out) const;

  static std::string self_dir();
  static std::string peers_dir();

 private:
  std::vector<AvatarEntry> photos_;
  std::string store_json_path() const;
};

}  // namespace nyx
