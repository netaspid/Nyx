#include "nyx/app.hpp"

#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/paths.hpp"
#include "nyx/util.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

namespace nyx {

namespace {

constexpr std::size_t kMaxNicknameLen = 64;

bool read_nickname(const ByteBuffer& data, std::size_t offset, std::size_t len,
                   std::string& out) {
  if (len > kMaxNicknameLen || offset + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + offset), len);
  return true;
}

}  // namespace

ByteBuffer HelloMessage::encode() const {
  ByteBuffer out;
  out.reserve(1 + kPublicKeySize + 2 + nickname.size() + 4);
  out.push_back(static_cast<uint8_t>(ChatKind::Hello));
  out.insert(out.end(), public_key.begin(), public_key.end());
  write_u16_le(out, static_cast<uint16_t>(nickname.size()));
  out.insert(out.end(), nickname.begin(), nickname.end());
  write_u32_le(out, capabilities);
  return out;
}

std::optional<HelloMessage> HelloMessage::decode(const ByteBuffer& data) {
  if (data.size() < 1 + kPublicKeySize + 2 + 4) return std::nullopt;
  if (data[0] != static_cast<uint8_t>(ChatKind::Hello)) return std::nullopt;

  HelloMessage msg;
  std::memcpy(msg.public_key.data(), data.data() + 1, kPublicKeySize);
  const uint16_t nick_len = read_u16_le(data.data() + 1 + kPublicKeySize);
  const std::size_t nick_off = 1 + kPublicKeySize + 2;
  if (!read_nickname(data, nick_off, nick_len, msg.nickname)) return std::nullopt;
  const std::size_t cap_off = nick_off + nick_len;
  if (cap_off + 4 > data.size()) return std::nullopt;
  msg.capabilities = read_u32_le(data.data() + cap_off);
  return msg;
}

ByteBuffer encode_text_message(const std::string& text) {
  ByteBuffer out;
  out.reserve(1 + text.size());
  out.push_back(static_cast<uint8_t>(ChatKind::Text));
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

std::optional<std::string> decode_text_message(const ByteBuffer& data) {
  if (data.empty() || data[0] != static_cast<uint8_t>(ChatKind::Text)) {
    return std::nullopt;
  }
  return std::string(data.begin() + 1, data.end());
}

std::optional<HelloMessage> decode_hello_message(const ByteBuffer& data) {
  return HelloMessage::decode(data);
}

bool exchange_hello(Connection& connection, const Profile& profile, HelloMessage& peer_out,
                    int timeout_sec, const std::function<bool()>& should_continue) {
  HelloMessage hello;
  hello.public_key = profile.public_key;
  hello.nickname = profile.nickname;
  if (!connection.send_payload(kChatStream, hello.encode())) return false;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    if (should_continue && !should_continue()) return false;
    connection.drive();
    ByteBuffer payload;
    uint32_t stream_id = 0;
    while (connection.recv_stream(stream_id, payload)) {
      if (stream_id != kChatStream) continue;
      if (auto decoded = decode_hello_message(payload)) {
        peer_out = std::move(*decoded);
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void remember_contact(const HelloMessage& peer) {
  ContactBook book(default_contacts_path());
  book.load();
  Contact contact;
  contact.user_id = peer.public_key;
  contact.nickname = peer.nickname;
  contact.last_seen_ms = now_ms();
  book.upsert(std::move(contact));
  book.save();
}

}  // namespace nyx
