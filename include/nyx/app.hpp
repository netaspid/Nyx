#pragma once

/** @file app.hpp
 *  Прикладные кадры на потоке чата: Hello (фаза 2), legacy Text.
 */

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/messaging.hpp"
#include "nyx/types.hpp"

#include <functional>
#include <optional>
#include <string>

namespace nyx {

/** Bit в HelloMessage::capabilities: следом идёт 32-байтный DM-inbox token. */
constexpr uint32_t kHelloCapDmInboxToken = 1u << 0;

/** Приветствие после handshake: публичный ключ, nickname, capabilities. */
struct HelloMessage {
  PublicKey public_key{};
  std::string nickname;
  uint32_t capabilities = 0;
  /** Стабильный inbox token отправителя (если capabilities & kHelloCapDmInboxToken). */
  InviteToken dm_inbox_token{};
  bool has_dm_inbox_token = false;

  ByteBuffer encode() const;
  static std::optional<HelloMessage> decode(const ByteBuffer& data);
};

/** Legacy Text-кадр (фаза 1). Новый код использует ChatMessage. */
ByteBuffer encode_text_message(const std::string& text);
std::optional<std::string> decode_text_message(const ByteBuffer& data);
std::optional<HelloMessage> decode_hello_message(const ByteBuffer& data);

/** Обмен Hello на kChatStream после Noise handshake. */
bool exchange_hello(Connection& connection, const Profile& profile, HelloMessage& peer_out,
                    int timeout_sec = 10,
                    const std::function<bool()>& should_continue = {});

/** Сохраняет контакт из Hello в books/contacts.json. */
void remember_contact(const HelloMessage& peer);

}  // namespace nyx
