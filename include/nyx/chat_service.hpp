#pragma once

/** @file chat_service.hpp
 *  AppCore API мессенджера (фаза 3): отправка, история, доставка, события.
 *  Используется CLI и будущим nyx-app (QML через тонкую обёртку).
 */

#include "nyx/app.hpp"
#include "nyx/chat_id.hpp"
#include "nyx/connection.hpp"
#include "nyx/identity.hpp"
#include "nyx/message_store.hpp"
#include "nyx/messaging.hpp"
#include "nyx/outbox.hpp"

#include <functional>
#include <string>
#include <vector>

namespace nyx {

/** Сессия чата 1:1 поверх установленного Connection. */
class ChatService {
 public:
  struct PeerInfo {
    UserId user_id{};
    std::string nickname;
  };

  using MessageCallback = std::function<void(const ChatMessage&, bool outgoing)>;
  using DeliveryCallback =
      std::function<void(uint64_t message_id, DeliveryStatus status)>;
  using EventCallback = std::function<void(const std::string& text)>;
  using CallFrameCallback = std::function<void(const ByteBuffer& frame)>;

  ChatService(Connection& connection, Profile profile, PeerInfo peer);

  /** Отправляет ChatMessage, ставит в outbox, сохраняет в историю. */
  bool send_message(const std::string& text, uint64_t* out_id = nullptr);

  /** Сигналинг звонка на kChatStream. */
  bool send_call_frame(const ByteBuffer& frame);

  /** Обрабатывает payload с kChatStream. */
  void handle_payload(const ByteBuffer& payload);

  /** Keep-alive + повтор исходящих без Ack. */
  void tick();

  /** Корректное отключение. */
  bool send_bye(const std::string& reason);

  std::vector<StoredMessage> history(std::size_t count) const;
  std::vector<StoredMessage> search(const std::string& query, std::size_t limit = 50) const;

  void set_on_message(MessageCallback cb) { on_message_ = std::move(cb); }
  void set_on_delivery(DeliveryCallback cb) { on_delivery_ = std::move(cb); }
  void set_on_event(EventCallback cb) { on_event_ = std::move(cb); }
  void set_on_call_frame(CallFrameCallback cb) { on_call_frame_ = std::move(cb); }

  const Profile& profile() const { return profile_; }
  const PeerInfo& peer() const { return peer_; }
  const ChatId& chat_id() const { return chat_id_; }
  MessageStore& store() { return store_; }
  Outbox& outbox() { return outbox_; }
  Connection& connection() { return connection_; }

  bool connected() const { return connected_; }

 private:
  ChatMessage make_message(const std::string& text) const;
  StoredMessage to_stored(const ChatMessage& msg, bool outgoing) const;
  void deliver_incoming(ChatMessage msg);
  void retry_pending();

  Connection& connection_;
  Profile profile_;
  PeerInfo peer_;
  ChatId chat_id_{};
  MessageStore store_;
  Outbox outbox_;
  bool connected_ = true;

  MessageCallback on_message_;
  DeliveryCallback on_delivery_;
  EventCallback on_event_;
  CallFrameCallback on_call_frame_;
};

}  // namespace nyx
