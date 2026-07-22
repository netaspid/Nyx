#include "nyx/chat_service.hpp"

#include <chrono>

#include "nyx/app.hpp"
#include "nyx/call_proto.hpp"
#include "nyx/util.hpp"

namespace nyx {

ChatService::ChatService(Connection& connection, Profile profile, PeerInfo peer)
    : connection_(connection),
      profile_(std::move(profile)),
      peer_(std::move(peer)),
      chat_id_(dm_chat_id(profile_.public_key, peer_.user_id)),
      store_(MessageStore::path_for_chat(chat_id_)) {}

ChatMessage ChatService::make_message(const std::string& text) const {
  ChatMessage msg;
  msg.id = next_message_id();
  msg.timestamp_ms = now_ms();
  msg.chat_id = chat_id_;
  msg.author_id = profile_.public_key;
  msg.author = profile_.nickname;
  msg.text = text;
  return msg;
}

StoredMessage ChatService::to_stored(const ChatMessage& msg, bool outgoing) const {
  StoredMessage stored;
  stored.id = msg.id;
  stored.timestamp_ms = msg.timestamp_ms;
  stored.chat_id_hex = chat_id_hex(msg.chat_id);
  stored.author = msg.author;
  stored.author_id_hex = to_hex(msg.author_id.data(), msg.author_id.size());
  stored.text = msg.text;
  stored.outgoing = outgoing;
  return stored;
}

bool ChatService::send_message(const std::string& text, uint64_t* out_id) {
  if (!connected_) return false;

  ChatMessage msg = make_message(text);
  const ByteBuffer wire = msg.encode();
  if (!connection_.send_payload(kChatStream, wire)) return false;

  PendingMessage pending;
  pending.message = msg;
  pending.wire = wire;
  outbox_.track(std::move(pending));
  store_.append(to_stored(msg, true));

  if (on_message_) on_message_(msg, true);
  if (out_id) *out_id = msg.id;
  return true;
}

bool ChatService::send_call_frame(const ByteBuffer& frame) {
  if (!connected_ || !is_call_frame(frame)) return false;
  return connection_.send_payload(kChatStream, frame);
}

void ChatService::deliver_incoming(ChatMessage msg) {
  store_.append(to_stored(msg, false));
  if (on_message_) on_message_(msg, false);

  AckMessage ack;
  ack.message_id = msg.id;
  connection_.send_payload(kChatStream, ack.encode());
}

void ChatService::handle_payload(const ByteBuffer& payload) {
  if (auto bye = ByeMessage::decode(payload)) {
    connected_ = false;
    const std::string reason =
        bye->reason.empty() ? "собеседник завершил сессию" : bye->reason;
    if (on_event_) on_event_(peer_.nickname + " отключился: " + reason);
    return;
  }

  if (auto ack = AckMessage::decode(payload)) {
    if (outbox_.on_ack(ack->message_id)) {
      if (on_delivery_) on_delivery_(ack->message_id, DeliveryStatus::Delivered);
    }
    return;
  }

  if (decode_hello_message(payload)) return;

  if (is_call_frame(payload)) {
    if (on_call_frame_) on_call_frame_(payload);
    return;
  }

  if (auto decoded = ChatMessage::decode(payload)) {
    deliver_incoming(std::move(*decoded));
    return;
  }

  if (auto legacy = decode_text_message(payload)) {
    ChatMessage msg = make_message(*legacy);
    msg.author_id = peer_.user_id;
    msg.author = peer_.nickname;
    deliver_incoming(std::move(msg));
    return;
  }

  if (!payload.empty() && on_event_) {
    on_event_("неизвестный кадр чата (" + std::to_string(payload.size()) + " байт)");
  }
}

void ChatService::retry_pending() {
  if (!connected_) return;

  const auto now = std::chrono::steady_clock::now();
  for (const uint64_t id : outbox_.due_for_retry(now)) {
    const PendingMessage* pending = outbox_.find(id);
    if (!pending) continue;

    if (!connection_.send_payload(kChatStream, pending->wire)) {
      connected_ = false;
      if (on_event_) on_event_("ошибка повторной отправки");
      return;
    }

    if (!outbox_.mark_retried(id, now)) {
      if (on_delivery_) on_delivery_(id, DeliveryStatus::Failed);
      if (on_event_) on_event_("не удалось доставить сообщение #" + std::to_string(id));
    }
  }
}

void ChatService::tick() {
  if (!connection_.drive()) {
    connected_ = false;
    if (on_event_) {
      if (!connection_.peer_alive()) {
        on_event_(peer_.nickname + " не отвечает (таймаут keep-alive)");
      } else {
        on_event_("соединение закрыто");
      }
    }
    return;
  }
  retry_pending();
}

bool ChatService::send_bye(const std::string& reason) {
  ByeMessage bye;
  bye.reason = reason;
  return connection_.send_payload(kChatStream, bye.encode());
}

std::vector<StoredMessage> ChatService::history(std::size_t count) const {
  return store_.recent(count);
}

std::vector<StoredMessage> ChatService::search(const std::string& query,
                                             std::size_t limit) const {
  return store_.search(query, limit);
}

}  // namespace nyx
