#pragma once

/** @file connection.hpp
 *  P2P connection: NAT (rendezvous), handshake, encryption, streams.
 */

#include "nyx/crypto.hpp"
#include "nyx/mux.hpp"
#include "nyx/proto.hpp"
#include "nyx/transport.hpp"
#include "nyx/types.hpp"
#include "nyx/udp.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>

namespace nyx {

/** Builds an EndpointHint for rendezvous registration. */
EndpointHint make_hint(const std::string& host, uint16_t port);

/** Bootstrap server client: register and lookup of an invite token. */
class RendezvousClient {
public:
  RendezvousClient(UdpSocket socket, std::string server_host, uint16_t server_port);

  /** Publishes the token and the current node UDP address. */
  bool register_token(const InviteToken& token);

  /** Looks up an address by token. Waits only for the rendezvous reply, hint.port != 0. */
  std::optional<EndpointHint> lookup(const InviteToken& token);

  UdpSocket& socket() { return socket_; }

private:
  bool send_msg(PacketType type, const ByteBuffer& payload);

  UdpSocket socket_;
  std::string server_host_;
  uint16_t server_port_;
};

/** Full P2P connection to one peer. */
class Connection {
public:
  Connection(UdpSocket socket, std::string peer_host, uint16_t peer_port);

  /** Outgoing connection: handshake initiator. */
  static std::optional<Connection> connect_initiator(UdpSocket socket,
                                                     const std::string& peer_host,
                                                     uint16_t peer_port,
                                                     int timeout_ms = 15000);

  /** Incoming: responder; first_packet is the already received HandshakeInit. */
  static std::optional<Connection> accept_responder(UdpSocket socket,
                                                    const std::string& peer_host,
                                                    uint16_t peer_port,
                                                    const ByteBuffer* first_packet,
                                                    int timeout_ms = 15000);

  ConnectionState state() const { return state_; }

  /** Ping on the control stream. */
  bool ping();

  /** Polls the network, keep-alive ping, peer timeout check. @return false when the peer is dead. */
  bool drive();

  /** Sends an arbitrary payload on a logical stream. */
  bool send_payload(uint32_t stream_id, const ByteBuffer& data);

  /**
   * Unreliable send on kRealtimeStream (PacketType::Realtime).
   * No ARQ/retransmit; meant for audio/video. Recommended size <= ~1100 bytes.
   */
  bool send_realtime(const ByteBuffer& data);

  /** Non-blocking receive of a realtime frame (after drive/feed_wire). */
  bool recv_realtime(ByteBuffer& out);

  /** false after a timeout with no peer response. */
  bool peer_alive() const { return peer_alive_; }

  /** Session rekey epoch (0 right after the handshake). */
  std::uint64_t session_rekey_epoch() const;

  const std::string& peer_host() const { return peer_host_; }
  uint16_t peer_port() const { return peer_port_; }

  /** Non-blocking receive: stream_id + data. */
  bool recv_stream(uint32_t& stream_id, ByteBuffer& out);

  /** Takes a payload from the mux without reading UDP (for the hub). */
  bool pop_stream(uint32_t& stream_id, ByteBuffer& out);

  /** Accepts a raw UDP frame (the hub dispatches per peer). */
  void feed_wire(const ByteBuffer& wire);

  /** Keep-alive without reading the socket (the hub dispatches recv itself). */
  bool drive_without_recv();

private:
  friend class PendingConnection;
  Connection(UdpSocket socket, std::string peer_host, uint16_t peer_port, Session session);

  void touch_peer_activity();
  bool process_incoming(int timeout_ms);
  bool run_handshake(HandshakeDriver& hs, const ByteBuffer* first_in);
  bool send_handshake(PacketType type, const ByteBuffer& payload);
  bool send_stream(uint32_t stream_id, const ByteBuffer& data, bool check_rekey = true);
  void flush_outbound();
  void process_wire(const ByteBuffer& wire);
  bool handle_realtime_wire(const Frame& frame);
  bool decrypt_dispatch(const ByteBuffer& cipher);
  void maybe_rekey();
  bool send_rekey(std::uint64_t epoch);
  bool apply_rekey_control(std::uint64_t epoch);

  UdpSocket socket_;
  std::string peer_host_;
  uint16_t peer_port_;
  ConnectionState state_ = ConnectionState::Handshaking;
  std::optional<Session> session_;
  ReliableSession reliable_;
  Multiplexer mux_;
  std::chrono::steady_clock::time_point last_peer_activity_ {};
  std::chrono::steady_clock::time_point last_ping_sent_ {};
  bool peer_alive_ = true;
  std::deque<ByteBuffer> outbound_wires_;
  std::deque<ByteBuffer> realtime_inbox_;
  uint32_t realtime_seq_ = 0;
};

/**
 * Stepwise Noise handshake on a shared UDP socket (mesh / demux).
 * Never calls recv_from; the socket owner feeds packets via feed_wire.
 */
class PendingConnection {
public:
  PendingConnection(UdpSocket socket,
                    std::string peer_host,
                    uint16_t peer_port,
                    HandshakeRole role);
  ~PendingConnection() = default;

  PendingConnection(const PendingConnection&) = delete;
  PendingConnection& operator=(const PendingConnection&) = delete;

  /** Sends the first step; a responder passes the already received HandshakeInit wire. */
  bool start(const ByteBuffer* first_wire = nullptr);

  /** Processes one UDP frame from the peer. @return true when the handshake completed. */
  bool feed_wire(const ByteBuffer& wire);

  bool complete() const { return complete_; }
  bool failed() const { return failed_; }
  const std::string& peer_host() const { return peer_host_; }
  uint16_t peer_port() const { return peer_port_; }

  /** Takes the Established Connection (only after completion). */
  std::optional<Connection> take();

private:
  bool send_hs(PacketType type, const ByteBuffer& payload);
  bool apply_payload(const ByteBuffer& hs_payload);

  UdpSocket socket_;
  std::string peer_host_;
  uint16_t peer_port_;
  HandshakeDriver hs_;
  bool complete_ = false;
  bool failed_ = false;
  bool started_ = false;
};

} // namespace nyx
