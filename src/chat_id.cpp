#include "nyx/chat_id.hpp"

#include "nyx/util.hpp"

#include <crypto/sha2/sha256.h>

#include <algorithm>
#include <cstring>

namespace nyx {

namespace {

void sha256_bytes(const uint8_t* data, std::size_t len, ChatId& out) {
  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update(&ctx, data, len);
  sha256_finish(&ctx, out.data());
}

}  // namespace

ChatId dm_chat_id(const UserId& self, const UserId& peer) {
  UserId lo = self;
  UserId hi = peer;
  if (std::memcmp(lo.data(), hi.data(), lo.size()) > 0) {
    std::swap(lo, hi);
  }
  uint8_t buf[kPublicKeySize * 2];
  std::memcpy(buf, lo.data(), kPublicKeySize);
  std::memcpy(buf + kPublicKeySize, hi.data(), kPublicKeySize);
  ChatId out{};
  sha256_bytes(buf, sizeof(buf), out);
  return out;
}

ChatId group_chat_id(const GroupId& group_id) {
  uint8_t prefix[] = {'g', 'r', 'o', 'u', 'p'};
  uint8_t buf[sizeof(prefix) + GroupId().size()];
  std::memcpy(buf, prefix, sizeof(prefix));
  std::memcpy(buf + sizeof(prefix), group_id.data(), group_id.size());
  ChatId out{};
  sha256_bytes(buf, sizeof(buf), out);
  return out;
}

std::string chat_id_hex(const ChatId& id) {
  return to_hex(id.data(), id.size());
}

}  // namespace nyx
