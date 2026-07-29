#pragma once

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


  ByteBuffer send(uint32_t stream_id, const ByteBuffer& data);


  std::optional<ByteBuffer> recv(uint32_t stream_id);


  ByteBuffer ping();


  std::vector<ByteBuffer> handle_control(const ByteBuffer& payload);


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

}
