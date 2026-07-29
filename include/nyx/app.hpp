#pragma once

/** @file app.hpp
 *  Application frames on the chat stream: Hello, legacy Text.
 */

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/messaging.hpp"
#include "nyx/profile_meta.hpp"
#include "nyx/types.hpp"

#include <functional>
#include <optional>
#include <string>

namespace nyx {

/** Bit in HelloMessage::capabilities: a 32-byte DM-inbox token follows. */
constexpr uint32_t kHelloCapDmInboxToken = 1u << 0;
/** After the optional inbox token: ProfileMeta (bio, interests, availability). */
constexpr uint32_t kHelloCapProfileMeta = 1u << 1;
/** Peer supports calls (CallKind on kChatStream). */
constexpr uint32_t kHelloCapCalls = 1u << 2;

/** Greeting after the handshake: public key, nickname, capabilities. */
struct HelloMessage {
  PublicKey public_key{};
  std::string nickname;
  uint32_t capabilities = 0;
  /** Stable sender inbox token (when capabilities & kHelloCapDmInboxToken). */
  InviteToken dm_inbox_token{};
  bool has_dm_inbox_token = false;
  ProfileMeta profile_meta{};
  bool has_profile_meta = false;

  ByteBuffer encode() const;
  static std::optional<HelloMessage> decode(const ByteBuffer& data);
};

/** Legacy Text frame. New code uses ChatMessage. */
ByteBuffer encode_text_message(const std::string& text);
std::optional<std::string> decode_text_message(const ByteBuffer& data);
std::optional<HelloMessage> decode_hello_message(const ByteBuffer& data);

/** Hello exchange on kChatStream after the Noise handshake. */
bool exchange_hello(Connection& connection, const Profile& profile, HelloMessage& peer_out,
                    int timeout_sec = 10,
                    const std::function<bool()>& should_continue = {});

/** Saves a contact from Hello into books/contacts.json. */
void remember_contact(const HelloMessage& peer);

}  // namespace nyx
