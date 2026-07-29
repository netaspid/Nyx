#pragma once

/** @file outbox.hpp
 *  Outgoing message queue awaiting Acks.
 */

#include "nyx/messaging.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nyx {

/** Application-level delivery status. */
enum class DeliveryStatus : uint8_t {
  Pending = 0,
  Delivered = 1,
  Failed = 2,
};

/** Outgoing message awaiting a peer Ack. */
struct PendingMessage {
  ChatMessage message;
  ByteBuffer wire;
  DeliveryStatus status = DeliveryStatus::Pending;
  int retries = 0;
  std::chrono::steady_clock::time_point sent_at{};
};

/** Tracks outgoing messages and resends on Ack timeout. */
class Outbox {
 public:
  static constexpr int kMaxRetries = 3;
  static constexpr std::chrono::milliseconds kAckTimeout{3000};

  void track(PendingMessage pending);

  /** Peer confirmed delivery. @return true when the id was queued. */
  bool on_ack(uint64_t message_id);

  /** Messages due for a resend. */
  std::vector<uint64_t> due_for_retry(std::chrono::steady_clock::time_point now) const;

  /** Bumps the retry counter; @return false when the limit is reached (Failed). */
  bool mark_retried(uint64_t message_id, std::chrono::steady_clock::time_point now);

  std::optional<DeliveryStatus> status(uint64_t message_id) const;
  const PendingMessage* find(uint64_t message_id) const;

  std::size_t pending_count() const;

 private:
  std::map<uint64_t, PendingMessage> pending_;
};

}  // namespace nyx
