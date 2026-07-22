#include "nyx/call_mesh.hpp"

#include "nyx/nat.hpp"
#include "nyx/proto.hpp"

#include <algorithm>

namespace nyx {

namespace {

constexpr std::size_t kVideoFullMeshMax = 8;
constexpr std::size_t kVideoSparseMax = 32;
constexpr std::size_t kVideoSparseTargets = 6;
constexpr auto kPunchInterval = std::chrono::milliseconds(200);
constexpr auto kConnectRetry = std::chrono::milliseconds(400);
constexpr auto kPendingTimeout = std::chrono::seconds(3);

}  // namespace

CallMesh::~CallMesh() { stop(); }

bool CallMesh::start(const CallId& call_id, const UserId& self) {
  std::lock_guard lock(mutex_);
  peers_.clear();
  socket_ = UdpSocket();
  if (!socket_.bind("0.0.0.0", 0)) {
    active_ = false;
    return false;
  }
  call_id_ = call_id;
  self_ = self;
  active_ = true;
  return true;
}

void CallMesh::stop() {
  std::lock_guard lock(mutex_);
  active_ = false;
  peers_.clear();
  socket_ = UdpSocket();
  call_id_ = {};
  self_ = {};
}

bool CallMesh::active() const {
  std::lock_guard lock(mutex_);
  return active_;
}

uint16_t CallMesh::local_port() const {
  std::lock_guard lock(mutex_);
  return socket_.local_port();
}

void CallMesh::set_on_realtime(RealtimeCallback cb) {
  std::lock_guard lock(mutex_);
  on_realtime_ = std::move(cb);
}

bool CallMesh::self_is_initiator(const UserId& self, const UserId& peer) {
  return self < peer;
}

std::string CallMesh::local_host_guess() const { return guess_lan_ipv4(); }

std::size_t CallMesh::established_count() const {
  std::lock_guard lock(mutex_);
  std::size_t n = 0;
  for (const auto& [_, link] : peers_) {
    if (link.conn && link.conn->state() == ConnectionState::Established) ++n;
  }
  return n;
}

std::size_t CallMesh::peer_count() const {
  std::lock_guard lock(mutex_);
  return peers_.size();
}

void CallMesh::upsert_peer(const CallPeerEndpoint& peer) {
  std::lock_guard lock(mutex_);
  if (!active_ || peer.port == 0 || peer.host.empty()) return;
  if (peer.user_id == self_) return;
  if (peers_.size() >= kMaxCallParticipants && peers_.find(peer.user_id) == peers_.end()) {
    return;
  }

  auto& link = peers_[peer.user_id];
  const bool addr_changed = link.ep.host != peer.host || link.ep.port != peer.port;
  link.ep = peer;

  if (link.conn && link.conn->state() == ConnectionState::Established && !addr_changed) {
    return;
  }
  if (addr_changed) {
    link.conn.reset();
    link.pending.reset();
    link.initiator_attempted = false;
    link.pending_since = {};
    link.last_connect_try = {};
    link.last_punch = {};
  }
}

void CallMesh::try_connect(PeerLink& link) {
  link.initiator_attempted = true;
  link.last_connect_try = std::chrono::steady_clock::now();
  link.pending_since = link.last_connect_try;
  link.pending = std::make_unique<PendingConnection>(socket_, link.ep.host, link.ep.port,
                                                     HandshakeRole::Initiator);
  if (!link.pending->start(nullptr)) {
    link.pending.reset();
    link.initiator_attempted = false;
  }
}

void CallMesh::maintain_peer(PeerLink& link, std::chrono::steady_clock::time_point now) {
  if (link.conn && link.conn->state() == ConnectionState::Established) return;
  if (link.ep.port == 0 || link.ep.host.empty()) return;

  if (now - link.last_punch >= kPunchInterval) {
    hole_punch_burst(socket_, make_hint(link.ep.host, link.ep.port), 4);
    link.last_punch = now;
  }

  if (link.pending) {
    if (link.pending->failed() || (now - link.pending_since >= kPendingTimeout)) {
      link.pending.reset();
      link.initiator_attempted = false;
    }
    return;
  }

  if (!self_is_initiator(self_, link.ep.user_id)) return;
  if (link.conn) return;
  if (now - link.last_connect_try < kConnectRetry && link.initiator_attempted) return;
  try_connect(link);
}

void CallMesh::remove_peer(const UserId& id) {
  std::lock_guard lock(mutex_);
  peers_.erase(id);
}

bool CallMesh::should_send_video_to(const UserId& peer) const {
  std::lock_guard lock(mutex_);
  const std::size_t n = peers_.size();
  if (n <= kVideoFullMeshMax) return true;
  if (n > kVideoSparseMax) return false;

  std::vector<UserId> ids;
  ids.reserve(peers_.size());
  for (const auto& [id, _] : peers_) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  const std::size_t take = std::min(kVideoSparseTargets, ids.size());
  auto it = std::lower_bound(ids.begin(), ids.end(), self_);
  std::vector<UserId> pick;
  pick.reserve(take);
  for (std::size_t i = 0; i < ids.size() && pick.size() < take; ++i) {
    const std::size_t idx =
        static_cast<std::size_t>((it - ids.begin() + static_cast<std::ptrdiff_t>(i)) %
                                 static_cast<std::ptrdiff_t>(ids.size()));
    pick.push_back(ids[idx]);
  }
  return std::find(pick.begin(), pick.end(), peer) != pick.end();
}

bool CallMesh::send_realtime_to(const UserId& peer, const ByteBuffer& data) {
  std::lock_guard lock(mutex_);
  auto it = peers_.find(peer);
  if (it == peers_.end() || !it->second.conn) return false;
  if (it->second.conn->state() != ConnectionState::Established) return false;
  return it->second.conn->send_realtime(data);
}

bool CallMesh::send_realtime(const ByteBuffer& data) {
  std::lock_guard lock(mutex_);
  bool any = false;
  for (auto& [_, link] : peers_) {
    if (!link.conn || link.conn->state() != ConnectionState::Established) continue;
    if (link.conn->send_realtime(data)) any = true;
  }
  return any;
}

bool CallMesh::send_realtime_video(const ByteBuffer& data) {
  std::lock_guard lock(mutex_);
  const std::size_t n = peers_.size();
  std::vector<UserId> video_targets;
  if (n <= kVideoFullMeshMax) {
    for (const auto& [id, _] : peers_) video_targets.push_back(id);
  } else if (n <= kVideoSparseMax) {
    std::vector<UserId> ids;
    for (const auto& [id, _] : peers_) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    const std::size_t take = std::min(kVideoSparseTargets, ids.size());
    auto it = std::lower_bound(ids.begin(), ids.end(), self_);
    for (std::size_t i = 0; i < ids.size() && video_targets.size() < take; ++i) {
      const std::size_t idx =
          static_cast<std::size_t>((it - ids.begin() + static_cast<std::ptrdiff_t>(i)) %
                                   static_cast<std::ptrdiff_t>(ids.size()));
      video_targets.push_back(ids[idx]);
    }
  }

  bool any = false;
  for (const auto& id : video_targets) {
    auto it = peers_.find(id);
    if (it == peers_.end() || !it->second.conn) continue;
    if (it->second.conn->state() != ConnectionState::Established) continue;
    if (it->second.conn->send_realtime(data)) any = true;
  }
  return any;
}

void CallMesh::demux_incoming(const std::string& host, uint16_t port, const ByteBuffer& wire) {
  if (is_punch_datagram(wire)) return;

  PeerLink* by_addr = nullptr;
  for (auto& [_, link] : peers_) {
    if (link.ep.host == host && link.ep.port == port) {
      by_addr = &link;
      break;
    }
  }

  if (is_handshake_datagram(wire)) {
    if (by_addr && by_addr->pending) {
      if (by_addr->pending->feed_wire(wire) && by_addr->pending->complete()) {
        if (auto conn = by_addr->pending->take()) {
          by_addr->conn = std::make_unique<Connection>(std::move(*conn));
        }
        by_addr->pending.reset();
      }
      return;
    }

    if (!by_addr) return;
    if (by_addr->conn && by_addr->conn->state() == ConnectionState::Established) return;
    if (self_is_initiator(self_, by_addr->ep.user_id)) return;

    by_addr->pending = std::make_unique<PendingConnection>(socket_, host, port,
                                                           HandshakeRole::Responder);
    by_addr->pending_since = std::chrono::steady_clock::now();
    if (!by_addr->pending->start(&wire)) {
      by_addr->pending.reset();
      return;
    }
    if (by_addr->pending->complete()) {
      if (auto conn = by_addr->pending->take()) {
        by_addr->conn = std::make_unique<Connection>(std::move(*conn));
      }
      by_addr->pending.reset();
    }
    return;
  }

  if (by_addr && by_addr->conn) {
    by_addr->conn->feed_wire(wire);
  }
}

void CallMesh::poll() {
  std::lock_guard lock(mutex_);
  if (!active_) return;

  std::string host;
  uint16_t port = 0;
  while (auto pkt = socket_.recv_from(host, port, 0)) {
    demux_incoming(host, port, *pkt);
  }

  const auto now = std::chrono::steady_clock::now();
  for (auto it = peers_.begin(); it != peers_.end();) {
    auto& link = it->second;
    maintain_peer(link, now);

    if (link.conn) {
      if (!link.conn->drive_without_recv()) {
        link.conn.reset();
        link.pending.reset();
        link.initiator_attempted = false;
        link.pending_since = {};
        link.last_connect_try = {};
        it = peers_.erase(it);
        continue;
      }
      ByteBuffer raw;
      while (link.conn->recv_realtime(raw)) {
        if (on_realtime_) on_realtime_(it->first, std::move(raw));
      }
    }
    ++it;
  }
}

}  // namespace nyx
