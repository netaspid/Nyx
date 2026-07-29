#pragma once

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

EndpointHint make_hint(const std::string& host, uint16_t port);

class RendezvousClient {
public:
  RendezvousClient(UdpSocket socket, std::string server_host, uint16_t server_port);


  bool register_token(const InviteToken& token);


  std::optional<EndpointHint> lookup(const InviteToken& token);

  UdpSocket& socket() { return socket_; }

private:
  bool send_msg(PacketType type, const ByteBuffer& payload);

  UdpSocket socket_;
  std::string server_host_;
  uint16_t server_port_;
};

class Connection {
public:
  Connection(UdpSocket socket, std::string peer_host, uint16_t peer_port);


  static std::optional<Connection> connect_initiator(UdpSocket socket,
                                                     const std::string& peer_host,
                                                     uint16_t peer_port,
                                                     int timeout_ms = 15000);


  static std::optional<Connection> accept_responder(UdpSocket socket,
                                                    const std::string& peer_host,
                                                    uint16_t peer_port,
                                                    const ByteBuffer* first_packet,
                                                    int timeout_ms = 15000);

  ConnectionState state() const { return state_; }


  bool ping();


  bool drive();


  bool send_payload(uint32_t stream_id, const ByteBuffer& data);


  bool send_realtime(const ByteBuffer& data);


  bool recv_realtime(ByteBuffer& out);


  bool peer_alive() const { return peer_alive_; }


  std::uint64_t session_rekey_epoch() const;

  const std::string& peer_host() const { return peer_host_; }
  uint16_t peer_port() const { return peer_port_; }


  bool recv_stream(uint32_t& stream_id, ByteBuffer& out);


  bool pop_stream(uint32_t& stream_id, ByteBuffer& out);


  void feed_wire(const ByteBuffer& wire);


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


  bool start(const ByteBuffer* first_wire = nullptr);


  bool feed_wire(const ByteBuffer& wire);

  bool complete() const { return complete_; }
  bool failed() const { return failed_; }
  const std::string& peer_host() const { return peer_host_; }
  uint16_t peer_port() const { return peer_port_; }


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

}
