#pragma once

/** @file call_av1.hpp
 *  AV1 encode/decode для видеозвонков (libaom).
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace nyx {

constexpr int kCallVideoWidth = 640;
constexpr int kCallVideoHeight = 360;
constexpr int kCallVideoFps = 12;
constexpr int kCallVideoTargetKbps = 900;  // legacy AV1; call path uses JPEG

/** Фрагмент видеокадра в CallMediaType::Video payload. */
struct CallVideoFragHeader {
  uint16_t frame_id = 0;
  uint8_t frag_index = 0;
  uint8_t frag_count = 1;
  uint8_t keyframe = 0;

  static constexpr std::size_t kSize = 5;
  void write(ByteBuffer& out) const;
  static std::optional<CallVideoFragHeader> read(const uint8_t* data, std::size_t len);
};

/** Нарезает AV1 OBU-буфер на фрагменты ≤ max_payload (с заголовком). */
std::vector<ByteBuffer> fragment_av1_frame(uint16_t frame_id, bool keyframe,
                                           const ByteBuffer& encoded,
                                           std::size_t max_payload);

/** Сборка фрагментов одного frame_id. */
class CallVideoReassembler {
 public:
  struct Assembled {
    ByteBuffer data;
    bool keyframe = false;
  };

  /** @return полный кадр когда все фрагменты собраны. */
  std::optional<Assembled> push(const ByteBuffer& frag_payload);

 private:
  uint16_t cur_id_ = 0;
  uint8_t expected_ = 0;
  bool keyframe_ = false;
  std::vector<ByteBuffer> parts_;
  std::vector<uint8_t> got_;
  bool active_ = false;
};

class Av1Encoder {
 public:
  Av1Encoder();
  ~Av1Encoder();
  Av1Encoder(const Av1Encoder&) = delete;
  Av1Encoder& operator=(const Av1Encoder&) = delete;

  bool ok() const { return ok_; }
  /** I420 (width*height*3/2) → AV1 OBU bytes. */
  std::optional<ByteBuffer> encode_i420(const uint8_t* i420, int width, int height,
                                        bool force_keyframe = false);

 private:
  void* codec_ = nullptr;  // aom_codec_ctx_t*
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

}  // namespace nyx
