#pragma once

/** @file identity.hpp
 *  Долгоживущая идентичность: Ed25519 ключи, nickname, профиль, контакты.
 */

#include "nyx/types.hpp"

#include <array>
#include <string>
#include <vector>

namespace nyx {

constexpr std::size_t kPublicKeySize = 32;
constexpr std::size_t kSecretKeySize = 32;

using PublicKey = std::array<uint8_t, kPublicKeySize>;
using SecretKey = std::array<uint8_t, kSecretKeySize>;
using UserId = PublicKey;

/** Профиль пользователя на диске. */
struct Profile {
  std::string nickname;
  SecretKey secret_key{};
  PublicKey public_key{};

  UserId user_id() const { return public_key; }
};

/** Короткий id для лога: первые 8 hex символов публичного ключа. */
std::string short_user_id(const UserId& id);

/** Генерирует новую пару ключей и nickname. */
Profile generate_profile(const std::string& nickname);

/** Загружает профиль или создаёт новый и сохраняет на диск. */
Profile load_or_create_profile(const std::string& path, const std::string& nickname);

/** Сохраняет профиль в JSON. */
bool save_profile(const std::string& path, const Profile& profile);

/** Загружает профиль. @return false если файла нет или формат неверный. */
bool load_profile(const std::string& path, Profile& out);

/** Запись в локальной книге контактов. */
struct Contact {
  UserId user_id{};
  std::string nickname;
  uint8_t trust_level = 0;
  uint64_t last_seen_ms = 0;
  /** Стабильный DM-inbox token peer (hex, 64); пусто если неизвестен. */
  std::string dm_inbox_token_hex;
};

/** Локальная книга контактов (JSON на диске). */
class ContactBook {
 public:
  explicit ContactBook(std::string path);

  bool load();
  bool save() const;

  void upsert(Contact contact);
  const std::vector<Contact>& contacts() const { return contacts_; }

 private:
  std::string path_;
  std::vector<Contact> contacts_;
};

}  // namespace nyx
