#pragma once

#include "nyx/profile_meta.hpp"
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

struct Profile {
  std::string nickname;
  SecretKey secret_key {};
  PublicKey public_key {};

  UserId user_id() const { return public_key; }
};

std::string short_user_id(const UserId& id);

Profile generate_profile(const std::string& nickname);

Profile load_or_create_profile(const std::string& path, const std::string& nickname);

bool save_profile(const std::string& path, const Profile& profile);

bool load_profile(const std::string& path, Profile& out);

struct Contact {
  UserId user_id {};
  std::string nickname;
  uint8_t trust_level = 0;
  uint64_t last_seen_ms = 0;

  std::string dm_inbox_token_hex;

  std::string bio;
  std::string interests;
  Availability availability = Availability::Available;

  std::vector<std::string> photo_hashes;
};

class ContactBook {
public:
  explicit ContactBook(std::string path);

  bool load();
  bool save() const;

  void upsert(Contact contact);

  bool remove(const UserId& user_id);
  const std::vector<Contact>& contacts() const { return contacts_; }

private:
  std::string path_;
  std::vector<Contact> contacts_;
};

} // namespace nyx
