#pragma once

/** @file profile_meta.hpp
 *  Публичная мета профиля: подпись, интересы, доступность.
 *  Локальный JSON; при handshake уходит в Hello (capability).
 */

#include "nyx/file_hash.hpp"
#include "nyx/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nyx {

constexpr std::size_t kMaxProfilePhotosWire = 5;

/** Статус доступности, которым делимся с peer. */
enum class Availability : uint8_t {
  Available = 0,
  Away = 1,
  Busy = 2,
  Invisible = 3,
};

/** Публичная карточка «о себе» (не секретные ключи). */
struct ProfileMeta {
  std::string bio;
  std::string interests;
  Availability availability = Availability::Available;
  uint64_t updated_ms = 0;
  /** Хеши фото (current = [0]), до kMaxProfilePhotosWire; в Hello. */
  std::vector<FileHash> photo_hashes;
};

/** Загрузка/сохранение data_dir()/profile_meta.json. */
bool load_profile_meta(ProfileMeta& out);
bool save_profile_meta(const ProfileMeta& meta);

std::string availability_to_string(Availability a);
Availability availability_from_string(const std::string& s);
std::string availability_label_ru(Availability a);

/** Сериализация меты в байты для Hello (после inbox token). */
void append_profile_meta_wire(ByteBuffer& out, const ProfileMeta& meta);
/** Разбор меты с offset; двигает offset. */
bool read_profile_meta_wire(const ByteBuffer& data, std::size_t& offset, ProfileMeta& out);

}  // namespace nyx
