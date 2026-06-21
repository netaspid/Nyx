#include "nyx/outbox.hpp"

namespace nyx {

void Outbox::track(PendingMessage pending) {
  pending.sent_at = std::chrono::steady_clock::now();
  pending.status = DeliveryStatus::Pending;
  pending.retries = 0;
  pending_[pending.message.id] = std::move(pending);
}

bool Outbox::on_ack(uint64_t message_id) {
  const auto it = pending_.find(message_id);
  if (it == pending_.end()) return false;
  it->second.status = DeliveryStatus::Delivered;
  pending_.erase(it);
  return true;
}

std::vector<uint64_t> Outbox::due_for_retry(
    std::chrono::steady_clock::time_point now) const {
  std::vector<uint64_t> ids;
  for (const auto& [id, msg] : pending_) {
    if (msg.status != DeliveryStatus::Pending) continue;
    if (now - msg.sent_at >= kAckTimeout) ids.push_back(id);
  }
  return ids;
}

bool Outbox::mark_retried(uint64_t message_id,
                          std::chrono::steady_clock::time_point now) {
  const auto it = pending_.find(message_id);
  if (it == pending_.end()) return false;
  ++it->second.retries;
  it->second.sent_at = now;
  if (it->second.retries >= kMaxRetries) {
    it->second.status = DeliveryStatus::Failed;
    pending_.erase(it);
    return false;
  }
  return true;
}

std::optional<DeliveryStatus> Outbox::status(uint64_t message_id) const {
  const auto it = pending_.find(message_id);
  if (it == pending_.end()) return std::nullopt;
  return it->second.status;
}

const PendingMessage* Outbox::find(uint64_t message_id) const {
  const auto it = pending_.find(message_id);
  return it == pending_.end() ? nullptr : &it->second;
}

std::size_t Outbox::pending_count() const { return pending_.size(); }

}  // namespace nyx
