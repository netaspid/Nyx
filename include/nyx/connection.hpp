#pragma once

/** @file connection.hpp
 *  Соединение P2P: NAT (rendezvous), handshake, шифрование, потоки.
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

/** Собирает EndpointHint для регистрации на rendezvous. */
EndpointHint make_hint(const std::string& host, uint16_t port);

/** Клиент bootstrap-сервера: register и lookup invite token. */
class RendezvousClient {
 public:
  RendezvousClient(UdpSocket socket, std::string server_host, uint16_t server_port);

  /** Публикует token и текущий UDP-адрес узла. */
  bool register_token(const InviteToken& token);

  /** Ищет адрес по token. Ждёт только ответ от rendezvous, hint.port != 0. */
  std::optional<EndpointHint> lookup(const InviteToken& token);

  UdpSocket& socket() { return socket_; }

 private:
  bool send_msg(PacketType type, const ByteBuffer& payload);

  UdpSocket socket_;
  std::string server_host_;
  uint16_t server_port_;
};

/** Полное P2P-соединение с одним peer. */
class Connection {
 public:
  Connection(UdpSocket socket, std::string peer_host, uint16_t peer_port);

  /** Исходящее соединение: handshake initiator. */
  static std::optional<Connection> connect_initiator(UdpSocket socket,
                                                     const std::string& peer_host,
                                                     uint16_t peer_port,
                                                     int timeout_ms = 15000);

  /** Входящее: responder, first_packet — уже принятый HandshakeInit. */
  static std::optional<Connection> accept_responder(
      UdpSocket socket, const std::string& peer_host, uint16_t peer_port,
      const ByteBuffer* first_packet, int timeout_ms = 15000);

  ConnectionState state() const { return state_; }

  /** Ping на control stream. */
  bool ping();

  /** Опрос сети, keep-alive ping, проверка таймаута peer. @return false если peer мёртв. */
  bool drive();

  /** Отправка текста на логический поток (UTF-8 байты). */
  bool send_text(uint32_t stream_id, const std::string& text);

  /** Отправка произвольной нагрузки на логический поток. */
  bool send_payload(uint32_t stream_id, const ByteBuffer& data);

  /** false после таймаута без ответа от peer. */
  bool peer_alive() const { return peer_alive_; }

  /** Epoch rekey сессии (0 после handshake). */
  std::uint64_t session_rekey_epoch() const;

  const std::string& peer_host() const { return peer_host_; }
  uint16_t peer_port() const { return peer_port_; }

  /** Неблокирующий приём: stream_id + данные. */
  bool recv_stream(uint32_t& stream_id, ByteBuffer& out);

  /** Забирает payload из mux без чтения UDP (для hub). */
  bool pop_stream(uint32_t& stream_id, ByteBuffer& out);

  /** Принимает сырой UDP-кадр (hub диспетчеризует по peer). */
  void feed_wire(const ByteBuffer& wire);

  /** Keep-alive без чтения с сокета (hub сам диспетчеризует recv). */
  bool drive_without_recv();

 private:
  void touch_peer_activity();
  bool process_incoming(int timeout_ms);
  bool run_handshake(HandshakeDriver& hs, const ByteBuffer* first_in);
  bool send_handshake(PacketType type, const ByteBuffer& payload);
  bool send_stream(uint32_t stream_id, const ByteBuffer& data, bool check_rekey = true);
  void flush_outbound();
  void process_wire(const ByteBuffer& wire);
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
  std::chrono::steady_clock::time_point last_peer_activity_{};
  std::chrono::steady_clock::time_point last_ping_sent_{};
  bool peer_alive_ = true;
  std::deque<ByteBuffer> outbound_wires_;
};

}  // namespace nyx
