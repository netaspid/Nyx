#pragma once

/** @file profile_crypto.hpp
 *  profile.nyx encryption: PBKDF2-HMAC-SHA256 + SHA256-stream XOR + HMAC-SHA256.
 *  The password/phrase is never stored; only the derived key lives in session memory.
 */

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

inline constexpr const char* recovery_vault_filename() {
  return "recovery.nyx";
}

/** Encrypts the profile with a password or a recovery phrase.
 *  @param derived_key_out optionally returns the session key (skips a second PBKDF2).
 */
bool save_encrypted_profile(const std::string& path,
                            const Profile& profile,
                            const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

/** Decrypts the profile. @return false on a wrong password or corrupted file. */
bool load_encrypted_profile(const std::string& path,
                            Profile& out,
                            const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

/** Rewrites the profile with the same derived key (active session). */
bool save_encrypted_profile_with_key(const std::string& path,
                                     const Profile& profile,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

/** Decrypts the profile with an already known derived key (remember-me). */
bool load_encrypted_profile_with_key(const std::string& path,
                                     Profile& out,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

/** Reads the KDF salt from an existing profile.nyx. */
bool read_profile_kdf_salt(const std::string& path, std::array<uint8_t, 16>& salt_out);

/** Minimum account password length. */
inline constexpr std::size_t kMinAccountPasswordLen = 8;

} // namespace nyx
