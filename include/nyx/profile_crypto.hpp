#pragma once

/** @file profile_crypto.hpp
 *  Шифрование profile.nyx: PBKDF2-HMAC-SHA256 + SHA256-stream XOR + HMAC-SHA256.
 *  Пароль/фраза не хранятся — только derived key в памяти сессии.
 */

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

inline constexpr const char* encrypted_profile_filename() { return "profile.nyx"; }
inline constexpr const char* recovery_vault_filename() { return "recovery.nyx"; }

/** Шифрует профиль паролем или recovery-фразой.
 *  @param derived_key_out опционально возвращает ключ сессии (без повторного PBKDF2).
 */
bool save_encrypted_profile(const std::string& path, const Profile& profile,
                            const std::string& password, std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

/** Расшифровывает профиль. @return false при неверном пароле или повреждённом файле. */
bool load_encrypted_profile(const std::string& path, Profile& out, const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

/** Перезаписывает профиль тем же derived key (активная сессия). */
bool save_encrypted_profile_with_key(const std::string& path, const Profile& profile,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

/** Расшифровывает профиль уже известным derived key (remember-me). */
bool load_encrypted_profile_with_key(const std::string& path, Profile& out,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

/** Читает salt KDF из существующего profile.nyx. */
bool read_profile_kdf_salt(const std::string& path, std::array<uint8_t, 16>& salt_out);

/** Минимальная длина пароля аккаунта. */
inline constexpr std::size_t kMinAccountPasswordLen = 8;

}  // namespace nyx
