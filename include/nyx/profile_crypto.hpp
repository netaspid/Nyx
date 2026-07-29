#pragma once

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

inline constexpr const char* recovery_vault_filename() {
  return "recovery.nyx";
}

bool save_encrypted_profile(const std::string& path,
                            const Profile& profile,
                            const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

bool load_encrypted_profile(const std::string& path,
                            Profile& out,
                            const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

bool save_encrypted_profile_with_key(const std::string& path,
                                     const Profile& profile,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

bool load_encrypted_profile_with_key(const std::string& path,
                                     Profile& out,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

bool read_profile_kdf_salt(const std::string& path, std::array<uint8_t, 16>& salt_out);

inline constexpr std::size_t kMinAccountPasswordLen = 8;

}
