#pragma once

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
  FileHash hash {};
  std::string mime = "image/jpeg";
  uint64_t set_ms = 0;
};

class AvatarStore {
public:
  AvatarStore();

  bool load();
  bool save() const;

  const std::vector<AvatarEntry>& photos() const { return photos_; }

  std::string path_for(const FileHash& hash) const;

  std::optional<AvatarEntry> current() const;


  bool set_from_file(const std::string& source_path);


  bool make_current(const FileHash& hash);

  bool remove(const FileHash& hash);


  bool cache_peer_photo(const UserId& peer,
                        const FileHash& hash,
                        const ByteBuffer& data,
                        const std::string& mime = "image/jpeg");

  std::string peer_path(const UserId& peer, const FileHash& hash) const;
  bool has_peer_photo(const UserId& peer, const FileHash& hash) const;


  bool read_bytes(const FileHash& hash, ByteBuffer& out) const;

  static std::string self_dir();
  static std::string peers_dir();

private:
  std::vector<AvatarEntry> photos_;
  std::string store_json_path() const;
};

}
