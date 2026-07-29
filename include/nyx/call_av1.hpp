#pragma once

#include "nyx/types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace nyx {

constexpr int kCallVideoWidth = 640;
constexpr int kCallVideoHeight = 360;
constexpr int kCallVideoFps = 12;
constexpr int kCallVideoTargetKbps = 900;

struct CallVideoFragHeader {
  uint16_t frame_id = 0;
  uint8_t frag_index = 0;
  uint8_t frag_count = 1;
  uint8_t keyframe = 0;

  static constexpr std::size_t kSize = 5;
  static constexpr uint8_t kKeyframe = 0x01;
  static constexpr uint8_t kParity = 0x02;
  void write(ByteBuffer& out) const;
  static std::optional<CallVideoFragHeader> read(const uint8_t* data, std::size_t len);
};

std::vector<ByteBuffer> fragment_av1_frame(uint16_t frame_id,
                                           bool keyframe,
                                           const ByteBuffer& encoded,
                                           std::size_t max_payload);

class CallVideoReassembler {
public:
  struct Assembled {
    ByteBuffer data;
    bool keyframe = false;
  };

  std::optional<Assembled> push(const ByteBuffer& frag_payload);

private:
  uint16_t cur_id_ = 0;
  uint8_t expected_ = 0;
  bool keyframe_ = false;
  std::vector<ByteBuffer> parts_;
  std::vector<uint8_t> got_;
  ByteBuffer parity_;
  std::size_t total_size_ = 0;
  bool active_ = false;
  std::chrono::steady_clock::time_point started_ {};
};

class Av1Encoder {
public:
  Av1Encoder();
  ~Av1Encoder();
  Av1Encoder(const Av1Encoder&) = delete;
  Av1Encoder& operator=(const Av1Encoder&) = delete;

  bool ok() const { return ok_; }

  std::optional<ByteBuffer>
  encode_i420(const uint8_t* i420, int width, int height, bool force_keyframe = false);

private:
  void* codec_ = nullptr;
  bool ok_ = false;
  int width_ = 0;
  int height_ = 0;
  unsigned pts_ = 0;
};

class Av1Decoder {
public:
  Av1Decoder();
  ~Av1Decoder();
  Av1Decoder(const Av1Decoder&) = delete;
  Av1Decoder& operator=(const Av1Decoder&) = delete;

  bool ok() const { return ok_; }

  struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> i420;
  };
  std::optional<Frame> decode(const uint8_t* data, std::size_t len);

private:
  void* codec_ = nullptr;
  bool ok_ = false;
};

} // namespace nyx
