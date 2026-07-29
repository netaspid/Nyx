#pragma once

/** @file identity.hpp
 *  Long-lived identity: Ed25519 keys, nickname, profile, contacts.
 */

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

/** User profile on disk. */
struct Profile {
  std::string nickname;
  SecretKey secret_key{};
  PublicKey public_key{};

  UserId user_id() const { return public_key; }
};

/** Short id for logs: first 8 hex chars of the public key. */
std::string short_user_id(const UserId& id);

/** Generates a new key pair and nickname. */
Profile generate_profile(const std::string& nickname);

/** Loads the profile, or creates and saves a new one. */
Profile load_or_create_profile(const std::string& path, const std::string& nickname);

bool save_profile(const std::string& path, const Profile& profile);

/** Loads the profile. @return false when the file is missing or malformed. */
bool load_profile(const std::string& path, Profile& out);

/** Local contact book record. */
struct Contact {
  UserId user_id{};
  std::string nickname;
  uint8_t trust_level = 0;
  uint64_t last_seen_ms = 0;
  /** Stable peer DM-inbox token (hex, 64); empty when unknown. */
  std::string dm_inbox_token_hex;
  /** Last known public meta (from Hello). */
  std::string bio;
  std::string interests;
  Availability availability = Availability::Available;
  /** Peer photo hashes (current = [0]), hex 64. */
  std::vector<std::string> photo_hashes;
};

/** Local contact book (JSON on disk). */
class ContactBook {
 public:
  explicit ContactBook(std::string path);

  bool load();
  bool save() const;

  void upsert(Contact contact);
  /** Removes a contact by user_id. @return true when the record existed. */
  bool remove(const UserId& user_id);
  const std::vector<Contact>& contacts() const { return contacts_; }

 private:
  std::string path_;
  std::vector<Contact> contacts_;
};

}  // namespace nyx
