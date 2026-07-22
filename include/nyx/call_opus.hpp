#pragma once

/** @file call_opus.hpp
 *  Opus encode/decode для звонков (48 kHz mono, 20 ms).
 */

#include "nyx/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace nyx {

constexpr int kCallAudioSampleRate = 48000;
constexpr int kCallAudioChannels = 1;
constexpr int kCallAudioFrameMs = 20;
constexpr int kCallAudioFrameSamples = kCallAudioSampleRate * kCallAudioFrameMs / 1000;

class OpusEncoderWrap {
 public:
  OpusEncoderWrap();
  ~OpusEncoderWrap();
  OpusEncoderWrap(const OpusEncoderWrap&) = delete;
  OpusEncoderWrap& operator=(const OpusEncoderWrap&) = delete;

  bool ok() const { return enc_ != nullptr; }
  /** PCM int16 mono → Opus packet. */
  std::optional<ByteBuffer> encode(const int16_t* pcm, int samples);

 private:
  void* enc_ = nullptr;
};

class OpusDecoderWrap {
 public:
  OpusDecoderWrap();
  ~OpusDecoderWrap();
  OpusDecoderWrap(const OpusDecoderWrap&) = delete;
  OpusDecoderWrap& operator=(const OpusDecoderWrap&) = delete;

  bool ok() const { return dec_ != nullptr; }
  /** Opus packet → PCM int16 (samples = kCallAudioFrameSamples при успехе). */
  std::optional<std::vector<int16_t>> decode(const uint8_t* data, std::size_t len);

 private:
  void* dec_ = nullptr;
};

}  // namespace nyx
