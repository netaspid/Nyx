#pragma once

/** @file call_mesh.hpp
 *  Mesh-медиасеть звонка: PeerIntro/Endpoint через хаб, медиа peer↔peer.
 */

#include "nyx/call_proto.hpp"
#include "nyx/connection.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nyx {

class CallMesh {
 public:
  using RealtimeCallback = std::function<void(const UserId& from, ByteBuffer frame)>;

  CallMesh() = default;
  ~CallMesh();

  CallMesh(const CallMesh&) = delete;
  CallMesh& operator=(const CallMesh&) = delete;

  bool start(const CallId& call_id, const UserId& self);
  void stop();
  bool active() const;

  uint16_t local_port() const;
  UdpSocket& socket() { return socket_; }

  void upsert_peer(const CallPeerEndpoint& peer);
  void remove_peer(const UserId& id);
  void poll();

  bool send_realtime(const ByteBuffer& data);
  bool send_realtime_video(const ByteBuffer& data);
  bool send_realtime_to(const UserId& peer, const ByteBuffer& data);

  void set_on_realtime(RealtimeCallback cb);

  std::size_t established_count() const;
  std::size_t peer_count() const;
  bool should_send_video_to(const UserId& peer) const;
  std::string local_host_guess() const;

 private:
  struct PeerLink {
    CallPeerEndpoint ep;
    std::unique_ptr<PendingConnection> pending;
    std::unique_ptr<Connection> conn;
    bool initiator_attempted = false;
    std::chrono::steady_clock::time_point pending_since{};
    std::chrono::steady_clock::time_point last_punch{};
    std::chrono::steady_clock::time_point last_connect_try{};
  };

  static bool self_is_initiator(const UserId& self, const UserId& peer);
  void try_connect(PeerLink& link);
  void maintain_peer(PeerLink& link, std::chrono::steady_clock::time_point now);
  void demux_incoming(const std::string& host, uint16_t port, const ByteBuffer& wire);

  mutable std::mutex mutex_;
  bool active_ = false;
  CallId call_id_{};
  UserId self_{};
  UdpSocket socket_;
  std::map<UserId, PeerLink> peers_;
  RealtimeCallback on_realtime_;
};

}  // namespace nyx
