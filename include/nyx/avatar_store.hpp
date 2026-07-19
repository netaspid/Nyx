#pragma once

/** @file avatar_store.hpp
 *  Локальные фото профиля (история до 5) и кэш peer-аватаров.
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

/** Свои аватары: current = photos[0]. */
class AvatarStore {
 public:
  AvatarStore();

  bool load();
  bool save() const;

  const std::vector<AvatarEntry>& photos() const { return photos_; }
  /** Путь к файлу своего аватара по hash; пусто если нет. */
  std::string path_for(const FileHash& hash) const;
  /** Текущее фото (первый в списке). */
  std::optional<AvatarEntry> current() const;

  /**
   * Добавляет JPEG/PNG с диска: копирует в store, считает hash, ставит первым.
   * FIFO при > kMaxAvatarHistory.
   */
  bool set_from_file(const std::string& source_path);

  /** Делает уже известный hash текущим (перемещает в начало). */
  bool make_current(const FileHash& hash);

  bool remove(const FileHash& hash);

  /** Кэш peer: сохранить блоб. */
  bool cache_peer_photo(const UserId& peer, const FileHash& hash, const ByteBuffer& data,
                        const std::string& mime = "image/jpeg");
  /** Путь к кэшу peer; пусто если нет файла. */
  std::string peer_path(const UserId& peer, const FileHash& hash) const;
  bool has_peer_photo(const UserId& peer, const FileHash& hash) const;

  /** Прочитать байты своего или peer-файла по hash (сначала self). */
  bool read_bytes(const FileHash& hash, ByteBuffer& out) const;

  static std::string self_dir();
  static std::string peers_dir();

 private:
  std::vector<AvatarEntry> photos_;
  std::string store_json_path() const;
};

}  // namespace nyx
