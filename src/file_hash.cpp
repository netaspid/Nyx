#include "nyx/file_hash.hpp"

#include "nyx/util.hpp"

#include <crypto/sha2/sha256.h>

#include <cstring>
#include <fstream>

namespace nyx {

namespace {

void sha256_update_buffer(sha256_context_t& ctx, const uint8_t* data, std::size_t len) {
  sha256_update(&ctx, data, len);
}

void sha256_finish_hash(sha256_context_t& ctx, FileHash& out) {
  sha256_finish(&ctx, out.data());
}

}  // namespace

bool hash_file(const std::string& path, FileHash& out) {
  std::ifstream file(path_from_utf8(path), std::ios::binary);
  if (!file) return false;

  sha256_context_t ctx;
  sha256_reset(&ctx);

  char buf[65536];
  while (file) {
    file.read(buf, sizeof(buf));
    const auto got = static_cast<std::size_t>(file.gcount());
    if (got == 0) break;
    sha256_update_buffer(ctx, reinterpret_cast<const uint8_t*>(buf), got);
  }
  sha256_finish_hash(ctx, out);
  return true;
}

FileHash hash_bytes(const uint8_t* data, std::size_t len) {
  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update_buffer(ctx, data, len);
  FileHash out{};
  sha256_finish_hash(ctx, out);
  return out;
}

std::string hash_hex(const FileHash& hash) { return to_hex(hash.data(), hash.size()); }

bool hash_from_hex(const std::string& hex, FileHash& out) {
  ByteBuffer bytes;
  if (!from_hex(hex, bytes) || bytes.size() != 32) return false;
  std::memcpy(out.data(), bytes.data(), 32);
  return true;
}

}  // namespace nyx
