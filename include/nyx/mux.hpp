#pragma once

/** @file mux.hpp
 *  Мультиплексор логических потоков внутри одного шифрованного канала.
 */

#include "nyx/proto.hpp"
#include "nyx/types.hpp"

#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace nyx {

class Multiplexer {
 public:
  Multiplexer();

  /** Выделяет новый stream_id (с 2, нечётные/чётные по соглашению позже). */
  uint32_t open_stream(StreamType type);

  /** Упаковка: u32 stream_id + payload для шифрования. */
  ByteBuffer send(uint32_t stream_id, const ByteBuffer& data);

  /** Забрать следующее сообщение из очереди потока. */
  std::optional<ByteBuffer> recv(uint32_t stream_id);

  /** Сформировать Ping для control stream. */
  ByteBuffer ping();

  /** Обработать control payload; может вернуть Pong и др. */
  std::vector<ByteBuffer> handle_control(const ByteBuffer& payload);

  /** Положить расшифрованные данные в очередь потока (приём). */
  void push(uint32_t stream_id, ByteBuffer data);

 private:
  struct Stream {
    StreamType type = StreamType::Data;
    std::deque<ByteBuffer> queue;
    bool open = true;
  };

  std::map<uint32_t, Stream> streams_;
  uint32_t next_stream_id_ = 2;
};

}  // namespace nyx
