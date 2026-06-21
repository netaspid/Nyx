#include "nyx/profile_crypto.hpp"

#include "nyx/util.hpp"

extern "C" {
#include <crypto/sha2/sha256.h>
}

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace nyx {

namespace {

constexpr char kProfileMagic[4] = {'N', 'Y', 'X', 'P'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint32_t kPbkdf2Iterations = 310000;
constexpr std::size_t kSaltSize = 16;
constexpr std::size_t kNonceSize = 12;
constexpr std::size_t kKeySize = 32;
constexpr std::size_t kMacSize = 32;

void hmac_sha256(const uint8_t* key, std::size_t key_len, const uint8_t* data,
                 std::size_t data_len, uint8_t out[32]) {
  std::array<uint8_t, 64> k{};
  if (key_len > 64) {
    sha256_context_t ctx;
    sha256_reset(&ctx);
    sha256_update(&ctx, key, key_len);
    sha256_finish(&ctx, k.data());
    key_len = 32;
    key = k.data();
  } else {
    std::memcpy(k.data(), key, key_len);
  }
  for (std::size_t i = 0; i < 64; ++i) k[i] ^= 0x36;

  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update(&ctx, k.data(), 64);
  sha256_update(&ctx, data, data_len);
  std::array<uint8_t, 32> inner{};
  sha256_finish(&ctx, inner.data());

  for (std::size_t i = 0; i < 64; ++i) k[i] ^= 0x36 ^ 0x5c;
  sha256_reset(&ctx);
  sha256_update(&ctx, k.data(), 64);
  sha256_update(&ctx, inner.data(), inner.size());
  sha256_finish(&ctx, out);
}

void pbkdf2_sha256(const std::string& password, const uint8_t* salt, std::size_t salt_len,
                   std::uint32_t iterations, uint8_t out[kKeySize]) {
  std::array<uint8_t, 32> u{};
  std::array<uint8_t, 32> t{};
  std::array<uint8_t, 36> block{};
  std::memcpy(block.data(), salt, salt_len);
  block[salt_len + 0] = 0;
  block[salt_len + 1] = 0;
  block[salt_len + 2] = 0;
  block[salt_len + 3] = 1;
  hmac_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(),
              block.data(), salt_len + 4, u.data());
  std::memcpy(t.data(), u.data(), 32);
  for (std::uint32_t i = 1; i < iterations; ++i) {
    hmac_sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size(), u.data(),
                32, u.data());
    for (int j = 0; j < 32; ++j) t[j] ^= u[j];
  }
  std::memcpy(out, t.data(), kKeySize);
}

void stream_xor(uint8_t* out, const uint8_t* in, std::size_t len, const uint8_t key[32],
                const uint8_t nonce[12]) {
  std::array<uint8_t, 32> block{};
  std::uint64_t counter = 0;
  std::size_t off = 0;
  while (off < len) {
    sha256_context_t ctx;
    sha256_reset(&ctx);
    sha256_update(&ctx, key, 32);
    sha256_update(&ctx, nonce, 12);
    const uint8_t cbuf[8] = {
        static_cast<uint8_t>(counter & 0xFF),
        static_cast<uint8_t>((counter >> 8) & 0xFF),
        static_cast<uint8_t>((counter >> 16) & 0xFF),
        static_cast<uint8_t>((counter >> 24) & 0xFF),
        static_cast<uint8_t>((counter >> 32) & 0xFF),
        static_cast<uint8_t>((counter >> 40) & 0xFF),
        static_cast<uint8_t>((counter >> 48) & 0xFF),
        static_cast<uint8_t>((counter >> 56) & 0xFF),
    };
    sha256_update(&ctx, cbuf, 8);
    sha256_finish(&ctx, block.data());
    for (std::size_t i = 0; i < 32 && off < len; ++i, ++off) {
      out[off] = static_cast<uint8_t>(in[off] ^ block[i]);
    }
    ++counter;
  }
}

bool mac_verify(const uint8_t key[32], const uint8_t* nonce, const uint8_t* cipher,
                std::size_t cipher_len, const uint8_t mac[kMacSize]) {
  std::vector<uint8_t> buf;
  buf.reserve(kNonceSize + cipher_len);
  buf.insert(buf.end(), nonce, nonce + kNonceSize);
  buf.insert(buf.end(), cipher, cipher + cipher_len);
  std::array<uint8_t, 32> calc{};
  hmac_sha256(key, 32, buf.data(), buf.size(), calc.data());
  uint8_t diff = 0;
  for (std::size_t i = 0; i < kMacSize; ++i) diff |= static_cast<uint8_t>(calc[i] ^ mac[i]);
  return diff == 0;
}

void mac_compute(const uint8_t key[32], const uint8_t* nonce, const uint8_t* cipher,
                 std::size_t cipher_len, uint8_t mac[kMacSize]) {
  std::vector<uint8_t> buf;
  buf.reserve(kNonceSize + cipher_len);
  buf.insert(buf.end(), nonce, nonce + kNonceSize);
  buf.insert(buf.end(), cipher, cipher + cipher_len);
  hmac_sha256(key, 32, buf.data(), buf.size(), mac);
}

std::string profile_plain_json(const Profile& profile) {
  return std::string("{\"v\":1,\"nickname\":\"") + profile.nickname + "\",\"sk\":\"" +
         to_hex(profile.secret_key.data(), profile.secret_key.size()) + "\",\"pk\":\"" +
         to_hex(profile.public_key.data(), profile.public_key.size()) + "\"}";
}

bool parse_profile_json(const std::string& json, Profile& out) {
  auto nick_pos = json.find("\"nickname\":\"");
  auto sk_pos = json.find("\"sk\":\"");
  auto pk_pos = json.find("\"pk\":\"");
  if (nick_pos == std::string::npos || sk_pos == std::string::npos || pk_pos == std::string::npos) {
    return false;
  }
  nick_pos += 12;
  const auto nick_end = json.find('"', nick_pos);
  sk_pos += 6;
  const auto sk_end = json.find('"', sk_pos);
  pk_pos += 6;
  const auto pk_end = json.find('"', pk_pos);
  if (nick_end == std::string::npos || sk_end == std::string::npos || pk_end == std::string::npos) {
    return false;
  }
  Profile p;
  p.nickname = json.substr(nick_pos, nick_end - nick_pos);
  std::vector<uint8_t> sk_bytes;
  std::vector<uint8_t> pk_bytes;
  if (!from_hex(json.substr(sk_pos, sk_end - sk_pos), sk_bytes) || sk_bytes.size() != 32) {
    return false;
  }
  if (!from_hex(json.substr(pk_pos, pk_end - pk_pos), pk_bytes) || pk_bytes.size() != 32) {
    return false;
  }
  std::memcpy(p.secret_key.data(), sk_bytes.data(), 32);
  std::memcpy(p.public_key.data(), pk_bytes.data(), 32);
  out = std::move(p);
  return true;
}

bool encrypt_profile_blob(const Profile& profile, const std::array<uint8_t, 32>& derived_key,
                          const uint8_t salt[kSaltSize], std::vector<uint8_t>& blob) {
  std::array<uint8_t, kNonceSize> nonce{};
  random_bytes(nonce.data(), nonce.size());

  const std::string plain = profile_plain_json(profile);
  std::vector<uint8_t> cipher(plain.size());
  stream_xor(cipher.data(), reinterpret_cast<const uint8_t*>(plain.data()), plain.size(),
             derived_key.data(), nonce.data());

  std::array<uint8_t, kMacSize> mac{};
  mac_compute(derived_key.data(), nonce.data(), cipher.data(), cipher.size(), mac.data());

  blob.clear();
  blob.insert(blob.end(), kProfileMagic, kProfileMagic + 4);
  blob.push_back(static_cast<uint8_t>(kFormatVersion & 0xFF));
  blob.push_back(static_cast<uint8_t>((kFormatVersion >> 8) & 0xFF));
  blob.push_back(1);
  write_u32_le(blob, kPbkdf2Iterations);
  blob.insert(blob.end(), salt, salt + kSaltSize);
  blob.insert(blob.end(), nonce.begin(), nonce.end());
  blob.insert(blob.end(), cipher.begin(), cipher.end());
  blob.insert(blob.end(), mac.begin(), mac.end());
  return true;
}

}  // namespace

bool save_encrypted_profile(const std::string& path, const Profile& profile,
                            const std::string& password, std::string* err) {
  if (password.size() < 8) {
    if (err) *err = "пароль должен быть не короче 8 символов";
    return false;
  }
  std::array<uint8_t, kSaltSize> salt{};
  random_bytes(salt.data(), salt.size());
  std::array<uint8_t, kKeySize> key{};
  pbkdf2_sha256(password, salt.data(), salt.size(), kPbkdf2Iterations, key.data());

  std::vector<uint8_t> blob;
  if (!encrypt_profile_blob(profile, key, salt.data(), blob)) return false;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "не удалось записать профиль";
    return false;
  }
  out.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
  return static_cast<bool>(out);
}

bool load_encrypted_profile(const std::string& path, Profile& out, const std::string& password,
                            std::string* err, std::array<uint8_t, 32>* derived_key_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "файл профиля не найден";
    return false;
  }
  std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::size_t header = 4 + 2 + 1 + 4 + kSaltSize + kNonceSize + kMacSize;
  if (blob.size() < header) {
    if (err) *err = "повреждённый профиль";
    return false;
  }
  if (std::memcmp(blob.data(), kProfileMagic, 4) != 0) {
    if (err) *err = "неизвестный формат профиля";
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(blob[4] | (static_cast<std::uint16_t>(blob[5]) << 8));
  if (version != kFormatVersion) {
    if (err) *err = "версия профиля не поддерживается";
    return false;
  }
  if (blob[6] != 1) {
    if (err) *err = "неизвестный KDF";
    return false;
  }
  const std::uint32_t iterations = read_u32_le(blob.data() + 7);
  if (iterations == 0) {
    if (err) *err = "повреждённый профиль";
    return false;
  }

  const uint8_t* salt = blob.data() + 11;
  const uint8_t* nonce = salt + kSaltSize;
  const uint8_t* mac = blob.data() + blob.size() - kMacSize;
  const uint8_t* cipher = nonce + kNonceSize;
  const std::size_t cipher_len = static_cast<std::size_t>(mac - cipher);

  std::array<uint8_t, kKeySize> key{};
  pbkdf2_sha256(password, salt, kSaltSize, iterations, key.data());
  if (derived_key_out) *derived_key_out = key;
  if (!mac_verify(key.data(), nonce, cipher, cipher_len, mac)) {
    if (err) *err = "неверный пароль";
    return false;
  }

  std::vector<uint8_t> plain(cipher_len);
  stream_xor(plain.data(), cipher, cipher_len, key.data(), nonce);

  const std::string json(reinterpret_cast<char*>(plain.data()), plain.size());
  if (!parse_profile_json(json, out)) {
    if (err) *err = "не удалось разобрать профиль";
    return false;
  }
  return true;
}

bool save_encrypted_profile_with_key(const std::string& path, const Profile& profile,
                                     const std::array<uint8_t, 32>& derived_key,
                                     std::string* err) {
  std::array<uint8_t, kSaltSize> salt{};
  if (!read_profile_kdf_salt(path, salt)) {
    random_bytes(salt.data(), salt.size());
  }
  std::vector<uint8_t> blob;
  if (!encrypt_profile_blob(profile, derived_key, salt.data(), blob)) return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "не удалось записать профиль";
    return false;
  }
  out.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
  return static_cast<bool>(out);
}

bool read_profile_kdf_salt(const std::string& path, std::array<uint8_t, 16>& salt_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::size_t need = 11 + kSaltSize;
  if (blob.size() < need) return false;
  if (std::memcmp(blob.data(), kProfileMagic, 4) != 0) return false;
  std::memcpy(salt_out.data(), blob.data() + 11, kSaltSize);
  return true;
}

}  // namespace nyx
