#include "nyx/call_av1.hpp"

#include "nyx/util.hpp"

#include <aom/aom_decoder.h>
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <aom/aomdx.h>

#include <algorithm>
#include <cstring>

namespace nyx {

void CallVideoFragHeader::write(ByteBuffer& out) const {
  write_u16_le(out, frame_id);
  out.push_back(frag_index);
  out.push_back(frag_count);
  out.push_back(keyframe);
}

std::optional<CallVideoFragHeader> CallVideoFragHeader::read(const uint8_t* data,
                                                             std::size_t len) {
  if (!data || len < kSize) return std::nullopt;
  CallVideoFragHeader h;
  h.frame_id = read_u16_le(data);
  h.frag_index = data[2];
  h.frag_count = data[3];
  h.keyframe = data[4];
  if (h.frag_count == 0 || h.frag_index >= h.frag_count) return std::nullopt;
  return h;
}

std::vector<ByteBuffer> fragment_av1_frame(uint16_t frame_id, bool keyframe,
                                           const ByteBuffer& encoded,
                                           std::size_t max_payload) {
  std::vector<ByteBuffer> out;
  if (encoded.empty() || max_payload <= CallVideoFragHeader::kSize) return out;
  const std::size_t chunk = max_payload - CallVideoFragHeader::kSize;
  const std::size_t n = (encoded.size() + chunk - 1) / chunk;
  if (n == 0 || n > 255) return out;

  for (std::size_t i = 0; i < n; ++i) {
    CallVideoFragHeader h;
    h.frame_id = frame_id;
    h.frag_index = static_cast<uint8_t>(i);
    h.frag_count = static_cast<uint8_t>(n);
    h.keyframe = keyframe ? 1 : 0;
    ByteBuffer frag;
    h.write(frag);
    const std::size_t off = i * chunk;
    const std::size_t len = std::min(chunk, encoded.size() - off);
    frag.insert(frag.end(), encoded.begin() + static_cast<std::ptrdiff_t>(off),
                encoded.begin() + static_cast<std::ptrdiff_t>(off + len));
    out.push_back(std::move(frag));
  }
  return out;
}

std::optional<ByteBuffer> CallVideoReassembler::push(const ByteBuffer& frag_payload) {
  auto h = CallVideoFragHeader::read(frag_payload.data(), frag_payload.size());
  if (!h) return std::nullopt;
  const auto* body = frag_payload.data() + CallVideoFragHeader::kSize;
  const std::size_t body_len = frag_payload.size() - CallVideoFragHeader::kSize;

  if (!active_ || h->frame_id != cur_id_ || h->frag_count != expected_) {
    active_ = true;
    cur_id_ = h->frame_id;
    expected_ = h->frag_count;
    keyframe_ = h->keyframe != 0;
    parts_.assign(expected_, ByteBuffer{});
    got_.assign(expected_, 0);
  }

  if (h->frag_index >= parts_.size()) {
    active_ = false;
    return std::nullopt;
  }
  parts_[h->frag_index].assign(body, body + body_len);
  got_[h->frag_index] = 1;

  for (uint8_t g : got_) {
    if (!g) return std::nullopt;
  }

  ByteBuffer full;
  for (auto& p : parts_) full.insert(full.end(), p.begin(), p.end());
  active_ = false;
  (void)keyframe_;
  return full;
}

Av1Encoder::Av1Encoder() {
  aom_codec_iface_t* iface = aom_codec_av1_cx();
  if (!iface) return;

  aom_codec_enc_cfg_t cfg;
  if (aom_codec_enc_config_default(iface, &cfg, AOM_USAGE_REALTIME) != AOM_CODEC_OK) return;

  width_ = kCallVideoWidth;
  height_ = kCallVideoHeight;
  cfg.g_w = static_cast<unsigned>(width_);
  cfg.g_h = static_cast<unsigned>(height_);
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = kCallVideoFps;
  cfg.rc_target_bitrate = kCallVideoTargetKbps;
  cfg.g_threads = 1;
  cfg.g_error_resilient = 0;
  cfg.g_lag_in_frames = 0;
  cfg.rc_end_usage = AOM_CBR;
  cfg.kf_mode = AOM_KF_AUTO;
  cfg.kf_max_dist = static_cast<unsigned>(kCallVideoFps * 2);

  auto* ctx = new aom_codec_ctx_t{};
  if (aom_codec_enc_init(ctx, iface, &cfg, 0) != AOM_CODEC_OK) {
    delete ctx;
    return;
  }
  aom_codec_control(ctx, AOME_SET_CPUUSED, 8);
  aom_codec_control(ctx, AV1E_SET_AQ_MODE, 3);
  aom_codec_control(ctx, AV1E_SET_TILE_COLUMNS, 0);
  aom_codec_control(ctx, AV1E_SET_TILE_ROWS, 0);
  codec_ = ctx;
  ok_ = true;
}

Av1Encoder::~Av1Encoder() {
  if (codec_) {
    aom_codec_destroy(static_cast<aom_codec_ctx_t*>(codec_));
    delete static_cast<aom_codec_ctx_t*>(codec_);
    codec_ = nullptr;
  }
}

std::optional<ByteBuffer> Av1Encoder::encode_i420(const uint8_t* i420, int width, int height,
                                                  bool force_keyframe) {
  if (!ok_ || !i420 || width != width_ || height != height_) return std::nullopt;
  auto* ctx = static_cast<aom_codec_ctx_t*>(codec_);

  aom_image_t* img = aom_img_alloc(nullptr, AOM_IMG_FMT_I420, width, height, 1);
  if (!img) return std::nullopt;
  const int y_sz = width * height;
  const int uv_w = width / 2;
  const int uv_h = height / 2;
  for (int row = 0; row < height; ++row) {
    std::memcpy(img->planes[0] + row * img->stride[0], i420 + row * width,
                static_cast<std::size_t>(width));
  }
  const uint8_t* u_src = i420 + y_sz;
  const uint8_t* v_src = u_src + uv_w * uv_h;
  for (int row = 0; row < uv_h; ++row) {
    std::memcpy(img->planes[1] + row * img->stride[1], u_src + row * uv_w,
                static_cast<std::size_t>(uv_w));
    std::memcpy(img->planes[2] + row * img->stride[2], v_src + row * uv_w,
                static_cast<std::size_t>(uv_w));
  }

  const aom_enc_frame_flags_t flags = force_keyframe ? AOM_EFLAG_FORCE_KF : 0;
  const aom_codec_err_t enc_err = aom_codec_encode(ctx, img, pts_++, 1, flags);
  aom_img_free(img);
  if (enc_err != AOM_CODEC_OK) return std::nullopt;

  ByteBuffer out;
  aom_codec_iter_t iter = nullptr;
  const aom_codec_cx_pkt_t* pkt = nullptr;
  while ((pkt = aom_codec_get_cx_data(ctx, &iter)) != nullptr) {
    if (pkt->kind != AOM_CODEC_CX_FRAME_PKT) continue;
    const auto* data = static_cast<const uint8_t*>(pkt->data.frame.buf);
    out.insert(out.end(), data, data + pkt->data.frame.sz);
  }
  if (out.empty()) return std::nullopt;
  return out;
}

Av1Decoder::Av1Decoder() {
  aom_codec_iface_t* iface = aom_codec_av1_dx();
  if (!iface) return;
  auto* ctx = new aom_codec_ctx_t{};
  aom_codec_dec_cfg_t cfg{};
  cfg.threads = 1;
  cfg.w = 0;
  cfg.h = 0;
  if (aom_codec_dec_init(ctx, iface, &cfg, 0) != AOM_CODEC_OK) {
    delete ctx;
    return;
  }
  codec_ = ctx;
  ok_ = true;
}

Av1Decoder::~Av1Decoder() {
  if (codec_) {
    aom_codec_destroy(static_cast<aom_codec_ctx_t*>(codec_));
    delete static_cast<aom_codec_ctx_t*>(codec_);
    codec_ = nullptr;
  }
}

std::optional<Av1Decoder::Frame> Av1Decoder::decode(const uint8_t* data, std::size_t len) {
  if (!ok_ || !data || len == 0) return std::nullopt;
  auto* ctx = static_cast<aom_codec_ctx_t*>(codec_);
  if (aom_codec_decode(ctx, data, len, nullptr) != AOM_CODEC_OK) return std::nullopt;

  aom_codec_iter_t iter = nullptr;
  aom_image_t* img = aom_codec_get_frame(ctx, &iter);
  if (!img) return std::nullopt;

  Frame f;
  f.width = static_cast<int>(img->d_w);
  f.height = static_cast<int>(img->d_h);
  const int y_sz = f.width * f.height;
  const int uv_sz = (f.width / 2) * (f.height / 2);
  f.i420.resize(static_cast<std::size_t>(y_sz + 2 * uv_sz));

  for (int row = 0; row < f.height; ++row) {
    std::memcpy(f.i420.data() + row * f.width, img->planes[0] + row * img->stride[0],
                static_cast<std::size_t>(f.width));
  }
  uint8_t* u = f.i420.data() + y_sz;
  uint8_t* v = u + uv_sz;
  for (int row = 0; row < f.height / 2; ++row) {
    std::memcpy(u + row * (f.width / 2), img->planes[1] + row * img->stride[1],
                static_cast<std::size_t>(f.width / 2));
    std::memcpy(v + row * (f.width / 2), img->planes[2] + row * img->stride[2],
                static_cast<std::size_t>(f.width / 2));
  }
  return f;
}

}  // namespace nyx
