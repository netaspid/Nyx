#pragma once

/** @file transport.hpp
 *  Надёжная доставка поверх UDP: selective repeat ARQ, фрагментация.
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace nyx {

class ReliableSession {
 public:
  explicit ReliableSession(std::size_t window = 256, std::size_t mtu = kDefaultMtu);

  /** Разбивает data на кадры Data для stream_id. */
  std::vector<ByteBuffer> send(uint32_t stream_id, const ByteBuffer& data);

  /** Принимает сырой UDP-буфер (кадр Nyx). */
  void recv_wire(const ByteBuffer& wire);

  /** Забирает следующее полностью собранное сообщение из очереди. */
  std::optional<ByteBuffer> poll_recv();

  /** Кадры Ack для отправки peer (SACK по out-of-order). */
  std::vector<ByteBuffer> make_ack_frames(uint32_t stream_id) const;

  /** Досылает фрагменты из очереди, когда в окне ARQ есть место. */
  std::vector<ByteBuffer> drain_outbound();

 private:
  struct SendItem {
    ByteBuffer payload;
    uint32_t retransmits = 0;
  };

  std::optional<ByteBuffer> encode_data(uint32_t stream_id, uint32_t seq,
                                        const ByteBuffer& payload) const;
  void on_ack(uint32_t ack, const std::vector<uint32_t>& sack);
  std::vector<ByteBuffer> on_data(uint32_t seq, const ByteBuffer& payload);

  std::size_t mtu_;
  std::size_t window_;
  uint32_t send_base_ = 0;
  uint32_t next_seq_ = 0;
  uint32_t recv_base_ = 0;
  uint32_t next_msg_id_ = 0;
  std::map<uint32_t, SendItem> inflight_;
  std::map<uint32_t, ByteBuffer> recv_buf_;
  std::vector<ByteBuffer> recv_queue_;

  struct Partial {
    uint16_t total = 0;
    std::vector<std::optional<ByteBuffer>> parts;
    uint16_t got = 0;
  };
  std::map<uint32_t, Partial> partials_;

  struct PendingSend {
    uint32_t stream_id = 0;
    std::vector<ByteBuffer> frags;
    std::size_t next = 0;
  };
  std::vector<PendingSend> pending_;

  std::vector<ByteBuffer> fragment(uint32_t msg_id, const ByteBuffer& data);
  struct AssembledMessage {
    uint32_t msg_id = 0;
    ByteBuffer data;
  };
  std::optional<AssembledMessage> assemble(const ByteBuffer& chunk);
  void enqueue_recv(uint32_t msg_id, ByteBuffer data);
  std::vector<ByteBuffer> flush_pending();

  uint32_t recv_msg_next_ = 0;
  std::map<uint32_t, ByteBuffer> recv_hold_;
};

}  // namespace nyx
