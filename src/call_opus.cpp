#include "nyx/call_opus.hpp"

#include <opus.h>

namespace nyx {

namespace {

OpusEncoder* as_enc(void* p) { return static_cast<OpusEncoder*>(p); }
OpusDecoder* as_dec(void* p) { return static_cast<OpusDecoder*>(p); }

}  // namespace

OpusEncoderWrap::OpusEncoderWrap() {
  int err = 0;
  OpusEncoder* enc =
      opus_encoder_create(kCallAudioSampleRate, kCallAudioChannels, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || !enc) {
    enc_ = nullptr;
    return;
  }
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
  opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
  opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
  enc_ = enc;
}

OpusEncoderWrap::~OpusEncoderWrap() {
  if (enc_) opus_encoder_destroy(as_enc(enc_));
}

std::optional<ByteBuffer> OpusEncoderWrap::encode(const int16_t* pcm, int samples) {
  if (!enc_ || !pcm || samples != kCallAudioFrameSamples) return std::nullopt;
  ByteBuffer out(400);
  const int n =
      opus_encode(as_enc(enc_), pcm, samples, out.data(), static_cast<opus_int32>(out.size()));
  if (n < 0) return std::nullopt;
  out.resize(static_cast<std::size_t>(n));
  return out;
}

OpusDecoderWrap::OpusDecoderWrap() {
  int err = 0;
  OpusDecoder* dec = opus_decoder_create(kCallAudioSampleRate, kCallAudioChannels, &err);
  if (err != OPUS_OK || !dec) {
    dec_ = nullptr;
    return;
  }
  dec_ = dec;
}

OpusDecoderWrap::~OpusDecoderWrap() {
  if (dec_) opus_decoder_destroy(as_dec(dec_));
}

std::optional<std::vector<int16_t>> OpusDecoderWrap::decode(const uint8_t* data, std::size_t len) {
  if (!dec_ || !data || len == 0) return std::nullopt;
  std::vector<int16_t> pcm(static_cast<std::size_t>(kCallAudioFrameSamples));
  const int n = opus_decode(as_dec(dec_), data, static_cast<opus_int32>(len), pcm.data(),
                            kCallAudioFrameSamples, 0);
  if (n < 0) return std::nullopt;
  pcm.resize(static_cast<std::size_t>(n));
  return pcm;
}

}  // namespace nyx
