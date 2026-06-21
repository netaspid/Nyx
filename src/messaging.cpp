#include "nyx/messaging.hpp"

#include "nyx/util.hpp"

#include <atomic>
#include <chrono>
#include <cstring>

namespace nyx {

namespace {

constexpr std::size_t kMaxNicknameLen = 64;
constexpr std::size_t kMaxTextLen = 65535;
constexpr std::size_t kMaxByeReasonLen = 256;

bool read_string(const ByteBuffer& data, std::size_t offset, std::size_t len,
                 std::size_t max_len, std::string& out) {
  if (len > max_len || offset + len > data.size()) return false;
  out.assign(reinterpret_cast<const char*>(data.data() + offset), len);
  return true;
}

std::optional<ChatMessage> decode_msg_body(const ByteBuffer& data, std::size_t off,
                                           bool has_chat_id) {
  if (data.size() < off + kPublicKeySize + 2 + 2) return std::nullopt;

  ChatMessage msg;
  if (has_chat_id) {
    if (data.size() < off + kPublicKeySize * 2 + 4) return std::nullopt;
    std::memcpy(msg.chat_id.data(), data.data() + off, kPublicKeySize);
    off += kPublicKeySize;
  }

  std::memcpy(msg.author_id.data(), data.data() + off, kPublicKeySize);
  off += kPublicKeySize;

  const uint16_t author_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, author_len, kMaxNicknameLen, msg.author)) {
    return std::nullopt;
  }
  off += author_len;
  if (off + 2 > data.size()) return std::nullopt;

  const uint16_t text_len = read_u16_le(data.data() + off);
  off += 2;
  if (!read_string(data, off, text_len, kMaxTextLen, msg.text)) return std::nullopt;
  return msg;
}

}  // namespace

uint64_t next_message_id() {
  static std::atomic<uint64_t> counter{
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count())};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

uint64_t now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

ByteBuffer ChatMessage::encode() const {
  ByteBuffer out;
  out.reserve(1 + 8 + 8 + kPublicKeySize * 2 + 2 + author.size() + 2 + text.size());
  out.push_back(static_cast<uint8_t>(ChatKind::MsgV2));
  write_u64_le(out, id);
  write_u64_le(out, timestamp_ms);
  out.insert(out.end(), chat_id.begin(), chat_id.end());
  out.insert(out.end(), author_id.begin(), author_id.end());
  write_u16_le(out, static_cast<uint16_t>(author.size()));
  out.insert(out.end(), author.begin(), author.end());
  write_u16_le(out, static_cast<uint16_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

std::optional<ChatMessage> ChatMessage::decode(const ByteBuffer& data) {
  if (data.empty()) return std::nullopt;

  const auto kind = static_cast<ChatKind>(data[0]);
  if (kind == ChatKind::MsgV2) {
    if (data.size() < 1 + 8 + 8) return std::nullopt;
    ChatMessage header;
    header.id = read_u64_le(data.data() + 1);
    header.timestamp_ms = read_u64_le(data.data() + 9);
    auto decoded = decode_msg_body(data, 17, true);
    if (!decoded) return std::nullopt;
    decoded->id = header.id;
    decoded->timestamp_ms = header.timestamp_ms;
    return decoded;
  }

  if (kind == ChatKind::Msg) {
    if (data.size() < 1 + 8 + 8) return std::nullopt;
    ChatMessage header;
    header.id = read_u64_le(data.data() + 1);
    header.timestamp_ms = read_u64_le(data.data() + 9);
    auto decoded = decode_msg_body(data, 17, false);
    if (!decoded) return std::nullopt;
    decoded->id = header.id;
    decoded->timestamp_ms = header.timestamp_ms;
    return decoded;
  }

  return std::nullopt;
}

ByteBuffer ByeMessage::encode() const {
  ByteBuffer out;
  out.reserve(1 + 2 + reason.size());
  out.push_back(static_cast<uint8_t>(ChatKind::Bye));
  write_u16_le(out, static_cast<uint16_t>(reason.size()));
  out.insert(out.end(), reason.begin(), reason.end());
  return out;
}

std::optional<ByeMessage> ByeMessage::decode(const ByteBuffer& data) {
  if (data.size() < 3 || data[0] != static_cast<uint8_t>(ChatKind::Bye)) {
    return std::nullopt;
  }
  const uint16_t len = read_u16_le(data.data() + 1);
  ByeMessage msg;
  if (!read_string(data, 3, len, kMaxByeReasonLen, msg.reason)) return std::nullopt;
  return msg;
}

ByteBuffer AckMessage::encode() const {
  ByteBuffer out;
  out.push_back(static_cast<uint8_t>(ChatKind::Ack));
  write_u64_le(out, message_id);
  return out;
}

std::optional<AckMessage> AckMessage::decode(const ByteBuffer& data) {
  if (data.size() < 9 || data[0] != static_cast<uint8_t>(ChatKind::Ack)) {
    return std::nullopt;
  }
  AckMessage msg;
  msg.message_id = read_u64_le(data.data() + 1);
  return msg;
}

}  // namespace nyx
