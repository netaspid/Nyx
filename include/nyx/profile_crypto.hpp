#pragma once

/** @file profile_crypto.hpp
 *  Шифрование профиля на диске (PBKDF2 + ChaCha20-Poly1305). Ключи только локально.
 */

#include "nyx/identity.hpp"

#include <array>
#include <string>

namespace nyx {

/** Имя файла зашифрованного профиля в каталоге аккаунта. */
inline constexpr const char* encrypted_profile_filename() { return "profile.nyx"; }

/** Сохраняет профиль в зашифрованном виде. Пароль не сохраняется. */
bool save_encrypted_profile(const std::string& path, const Profile& profile,
                            const std::string& password, std::string* err = nullptr);

/** Расшифровывает профиль. @return false при неверном пароле или повреждённом файле. */
bool load_encrypted_profile(const std::string& path, Profile& out, const std::string& password,
                            std::string* err = nullptr,
                            std::array<uint8_t, 32>* derived_key_out = nullptr);

/** Перезаписывает профиль тем же derived key (сессия). */
bool save_encrypted_profile_with_key(const std::string& path, const Profile& profile,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err = nullptr);

/** Читает salt KDF из существующего profile.nyx (offset 11). */
bool read_profile_kdf_salt(const std::string& path, std::array<uint8_t, 16>& salt_out);

}  // namespace nyx
