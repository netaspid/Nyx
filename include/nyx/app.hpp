#pragma once

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/messaging.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/types.hpp"

#include <functional>
#include <optional>
#include <string>

namespace nyx {

constexpr uint32_t kHelloCapDmInboxToken = 1u << 0;

constexpr uint32_t kHelloCapProfileMeta = 1u << 1;

constexpr uint32_t kHelloCapCalls = 1u << 2;

struct HelloMessage {
  PublicKey public_key {};
  std::string nickname;
  uint32_t capabilities = 0;

  InviteToken dm_inbox_token {};
  bool has_dm_inbox_token = false;
  ProfileMeta profile_meta {};
  bool has_profile_meta = false;

  ByteBuffer encode() const;
  static std::optional<HelloMessage> decode(const ByteBuffer& data);
};

ByteBuffer encode_text_message(const std::string& text);
std::optional<std::string> decode_text_message(const ByteBuffer& data);
std::optional<HelloMessage> decode_hello_message(const ByteBuffer& data);

/** Hello exchange on kChatStream after the Noise handshake. */
bool exchange_hello(Connection& connection,
                    const Profile& profile,
                    HelloMessage& peer_out,
                    int timeout_sec = 10,
                    const std::function<bool()>& should_continue = {});

void remember_contact(const HelloMessage& peer);

} // namespace nyx
