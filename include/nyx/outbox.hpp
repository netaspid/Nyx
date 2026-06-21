#pragma once

/** @file outbox.hpp
 *  Очередь исходящих сообщений с ожиданием Ack (фаза 3).
 */

#include "nyx/messaging.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nyx {

/** Статус доставки на уровне приложения. */
enum class DeliveryStatus : uint8_t {
  Pending = 0,
  Delivered = 1,
  Failed = 2,
};

/** Исходящее сообщение, ожидающее Ack от peer. */
struct PendingMessage {
  ChatMessage message;
  ByteBuffer wire;
  DeliveryStatus status = DeliveryStatus::Pending;
  int retries = 0;
  std::chrono::steady_clock::time_point sent_at{};
};

/** Отслеживает исходящие Msg и повторную отправку при таймауте Ack. */
class Outbox {
 public:
  static constexpr int kMaxRetries = 3;
  static constexpr std::chrono::milliseconds kAckTimeout{3000};

  /** Регистрирует отправленное сообщение. */
  void track(PendingMessage pending);

  /** Peer подтвердил доставку. @return true если id был в очереди. */
  bool on_ack(uint64_t message_id);

  /** Сообщения, которым пора повторить отправку. */
  std::vector<uint64_t> due_for_retry(std::chrono::steady_clock::time_point now) const;

  /** Увеличивает счётчик retry; @return false если лимит исчерпан (Failed). */
  bool mark_retried(uint64_t message_id, std::chrono::steady_clock::time_point now);

  std::optional<DeliveryStatus> status(uint64_t message_id) const;
  const PendingMessage* find(uint64_t message_id) const;

  std::size_t pending_count() const;

 private:
  std::map<uint64_t, PendingMessage> pending_;
};

}  // namespace nyx
