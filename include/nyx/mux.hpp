#pragma once

/** @file mux.hpp
 *  Multiplexer of logical streams inside one encrypted channel.
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


  /** Packs u32 stream_id + payload for encryption. */
  ByteBuffer send(uint32_t stream_id, const ByteBuffer& data);

  /** Takes the next message from the stream queue. */
  std::optional<ByteBuffer> recv(uint32_t stream_id);

  /** Builds a Ping for the control stream. */
  ByteBuffer ping();

  /** Handles a control payload; may return a Pong or similar. */
  std::vector<ByteBuffer> handle_control(const ByteBuffer& payload);

  /** Puts decrypted data into the stream queue (receive side). */
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
