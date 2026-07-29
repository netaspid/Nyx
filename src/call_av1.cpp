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

std::optional<CallVideoFragHeader> CallVideoFragHeader::read(const uint8_t* data, std::size_t len) {
  if (!data || len < kSize)
    return std::nullopt;
  CallVideoFragHeader h;
  h.frame_id = read_u16_le(data);
  h.frag_index = data[2];
  h.frag_count = data[3];
  h.keyframe = data[4];
  const bool parity = (h.keyframe & kParity) != 0;
  if (h.frag_count == 0 || (!parity && h.frag_index >= h.frag_count) ||
      (parity && h.frag_index != h.frag_count))
    return std::nullopt;
  return h;
}

std::vector<ByteBuffer> fragment_av1_frame(uint16_t frame_id,
                                           bool keyframe,
                                           const ByteBuffer& encoded,
                                           std::size_t max_payload) {
  std::vector<ByteBuffer> out;
  if (encoded.empty() || max_payload <= CallVideoFragHeader::kSize + 4)
    return out;

  const std::size_t chunk = max_payload - CallVideoFragHeader::kSize - 4;
  const std::size_t n = (encoded.size() + chunk - 1) / chunk;
  if (n == 0 || n > 255)
    return out;

  CallVideoFragHeader ph;
  ph.frame_id = frame_id;
  ph.frag_index = static_cast<uint8_t>(n);
  ph.frag_count = static_cast<uint8_t>(n);
  ph.keyframe = static_cast<uint8_t>((keyframe ? CallVideoFragHeader::kKeyframe : 0) |
                                     CallVideoFragHeader::kParity);
  ByteBuffer parity;
  ph.write(parity);
  write_u32_le(parity, static_cast<uint32_t>(encoded.size()));
  parity.resize(CallVideoFragHeader::kSize + 4 + chunk, 0);
  for (std::size_t i = 0; i < encoded.size(); ++i)
    parity[CallVideoFragHeader::kSize + 4 + (i % chunk)] ^= encoded[i];
  out.push_back(std::move(parity));

  for (std::size_t i = 0; i < n; ++i) {
    CallVideoFragHeader h;
    h.frame_id = frame_id;
    h.frag_index = static_cast<uint8_t>(i);
    h.frag_count = static_cast<uint8_t>(n);
    h.keyframe = keyframe ? CallVideoFragHeader::kKeyframe : 0;
    ByteBuffer frag;
    h.write(frag);
    const std::size_t off = i * chunk;
    const std::size_t len = std::min(chunk, encoded.size() - off);
    frag.insert(frag.end(),
                encoded.begin() + static_cast<std::ptrdiff_t>(off),
                encoded.begin() + static_cast<std::ptrdiff_t>(off + len));
    out.push_back(std::move(frag));
  }
  return out;
}

std::optional<CallVideoReassembler::Assembled>
CallVideoReassembler::push(const ByteBuffer& frag_payload) {
  auto h = CallVideoFragHeader::read(frag_payload.data(), frag_payload.size());
  if (!h)
    return std::nullopt;
  const auto* body = frag_payload.data() + CallVideoFragHeader::kSize;
  const std::size_t body_len = frag_payload.size() - CallVideoFragHeader::kSize;
  const auto now = std::chrono::steady_clock::now();
  const bool parity = (h->keyframe & CallVideoFragHeader::kParity) != 0;

  if (active_ && h->frame_id != cur_id_) {

    const uint16_t delta = static_cast<uint16_t>(cur_id_ - h->frame_id);
    const bool stalled = (now - started_) > std::chrono::milliseconds(150);
    if (delta != 0 && delta < 0x8000 && !stalled)
      return std::nullopt;
  }

  if (!active_ || h->frame_id != cur_id_ || h->frag_count != expected_) {
    active_ = true;
    cur_id_ = h->frame_id;
    expected_ = h->frag_count;
    keyframe_ = (h->keyframe & CallVideoFragHeader::kKeyframe) != 0;
    parts_.assign(expected_, ByteBuffer {});
    got_.assign(expected_, 0);
    parity_.clear();
    total_size_ = 0;
    started_ = now;
  }

  if (parity) {
    if (body_len < 4)
      return std::nullopt;
    total_size_ = read_u32_le(body);
    parity_.assign(body + 4, body + body_len);
  } else {
    if (h->frag_index >= parts_.size()) {
      active_ = false;
      return std::nullopt;
    }
    parts_[h->frag_index].assign(body, body + body_len);
    got_[h->frag_index] = 1;
  }

  int missing = -1;
  for (std::size_t i = 0; i < got_.size(); ++i) {
    if (!got_[i]) {
      if (missing >= 0)
        return std::nullopt;
      missing = static_cast<int>(i);
    }
  }
  if (missing >= 0) {
    if (parity_.empty() || total_size_ == 0)
      return std::nullopt;
    ByteBuffer recovered = parity_;
    for (std::size_t i = 0; i < parts_.size(); ++i) {
      if (static_cast<int>(i) == missing)
        continue;
      for (std::size_t j = 0; j < parts_[i].size() && j < recovered.size(); ++j)
        recovered[j] ^= parts_[i][j];
    }
    const std::size_t chunk = parity_.size();
    const std::size_t off = static_cast<std::size_t>(missing) * chunk;
    if (off >= total_size_)
      return std::nullopt;
    recovered.resize(std::min(chunk, total_size_ - off));
    parts_[static_cast<std::size_t>(missing)] = std::move(recovered);
    got_[static_cast<std::size_t>(missing)] = 1;
  }

  Assembled out;
  out.keyframe = keyframe_;
  for (auto& p : parts_)
    out.data.insert(out.data.end(), p.begin(), p.end());
  active_ = false;
  return out;
}

Av1Encoder::Av1Encoder() {
  aom_codec_iface_t* iface = aom_codec_av1_cx();
  if (!iface)
    return;

  aom_codec_enc_cfg_t cfg;
  if (aom_codec_enc_config_default(iface, &cfg, AOM_USAGE_REALTIME) != AOM_CODEC_OK)
    return;

  width_ = kCallVideoWidth;
  height_ = kCallVideoHeight;
  cfg.g_w = static_cast<unsigned>(width_);
  cfg.g_h = static_cast<unsigned>(height_);
  cfg.g_profile = 0;
  cfg.g_bit_depth = AOM_BITS_8;
  cfg.g_input_bit_depth = 8;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = kCallVideoFps;
  cfg.rc_target_bitrate = kCallVideoTargetKbps;
  cfg.g_threads = 4;
  cfg.g_error_resilient = 1;
  cfg.g_lag_in_frames = 0;
  cfg.rc_end_usage = AOM_CBR;
  cfg.kf_mode = AOM_KF_AUTO;
  cfg.kf_max_dist = static_cast<unsigned>(kCallVideoFps);

  auto* ctx = new aom_codec_ctx_t {};
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

std::optional<ByteBuffer>
Av1Encoder::encode_i420(const uint8_t* i420, int width, int height, bool force_keyframe) {
  if (!ok_ || !i420 || width != width_ || height != height_)
    return std::nullopt;
  auto* ctx = static_cast<aom_codec_ctx_t*>(codec_);

  aom_image_t* img = aom_img_alloc(nullptr, AOM_IMG_FMT_I420, width, height, 1);
  if (!img)
    return std::nullopt;
  const int y_sz = width * height;
  const int uv_w = width / 2;
  const int uv_h = height / 2;
  for (int row = 0; row < height; ++row) {
    std::memcpy(
        img->planes[0] + row * img->stride[0], i420 + row * width, static_cast<std::size_t>(width));
  }
  const uint8_t* u_src = i420 + y_sz;
  const uint8_t* v_src = u_src + uv_w * uv_h;
  for (int row = 0; row < uv_h; ++row) {
    std::memcpy(
        img->planes[1] + row * img->stride[1], u_src + row * uv_w, static_cast<std::size_t>(uv_w));
    std::memcpy(
        img->planes[2] + row * img->stride[2], v_src + row * uv_w, static_cast<std::size_t>(uv_w));
  }

  const aom_enc_frame_flags_t flags = force_keyframe ? AOM_EFLAG_FORCE_KF : 0;
  const aom_codec_err_t enc_err = aom_codec_encode(ctx, img, pts_++, 1, flags);
  aom_img_free(img);
  if (enc_err != AOM_CODEC_OK)
    return std::nullopt;

  ByteBuffer out;
  aom_codec_iter_t iter = nullptr;
  const aom_codec_cx_pkt_t* pkt = nullptr;
  while ((pkt = aom_codec_get_cx_data(ctx, &iter)) != nullptr) {
    if (pkt->kind != AOM_CODEC_CX_FRAME_PKT)
      continue;
    const auto* data = static_cast<const uint8_t*>(pkt->data.frame.buf);
    out.insert(out.end(), data, data + pkt->data.frame.sz);
  }
  if (out.empty())
    return std::nullopt;
  return out;
}

Av1Decoder::Av1Decoder() {
  aom_codec_iface_t* iface = aom_codec_av1_dx();
  if (!iface)
    return;
  auto* ctx = new aom_codec_ctx_t {};
  aom_codec_dec_cfg_t cfg {};
  cfg.threads = 2;
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
  if (!ok_ || !data || len == 0)
    return std::nullopt;
  auto* ctx = static_cast<aom_codec_ctx_t*>(codec_);
  if (aom_codec_decode(ctx, data, len, nullptr) != AOM_CODEC_OK)
    return std::nullopt;

  aom_codec_iter_t iter = nullptr;
  aom_image_t* img = aom_codec_get_frame(ctx, &iter);
  if (!img)
    return std::nullopt;

  Frame f;
  f.width = static_cast<int>(img->d_w);
  f.height = static_cast<int>(img->d_h);
  if (img->x_chroma_shift != 1 || img->y_chroma_shift != 1)
    return std::nullopt;
  const int y_sz = f.width * f.height;
  const int uv_sz = (f.width / 2) * (f.height / 2);
  f.i420.resize(static_cast<std::size_t>(y_sz + 2 * uv_sz));

  const bool high_bit_depth = (img->fmt & AOM_IMG_FMT_HIGHBITDEPTH) != 0;
  const int shift = high_bit_depth ? std::max(0, static_cast<int>(img->bit_depth) - 8) : 0;
  auto copy_plane =
      [high_bit_depth, shift](
          uint8_t* dst, int dst_stride, const uint8_t* src, int src_stride, int width, int height) {
        if (!dst || !src)
          return false;
        for (int row = 0; row < height; ++row) {
          const uint8_t* src_row = src + row * src_stride;
          uint8_t* dst_row = dst + row * dst_stride;
          if (!high_bit_depth) {
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(width));
            continue;
          }
          for (int col = 0; col < width; ++col) {
            uint16_t sample = 0;
            std::memcpy(&sample, src_row + col * 2, sizeof(sample));
            dst_row[col] = static_cast<uint8_t>(
                std::min<unsigned>(255, static_cast<unsigned>(sample >> shift)));
          }
        }
        return true;
      };

  if (!copy_plane(f.i420.data(), f.width, img->planes[0], img->stride[0], f.width, f.height))
    return std::nullopt;
  uint8_t* u = f.i420.data() + y_sz;
  uint8_t* v = u + uv_sz;
  const bool uv_flipped = (img->fmt & AOM_IMG_FMT_UV_FLIP) != 0;
  const int u_index = uv_flipped ? 2 : 1;
  const int v_index = uv_flipped ? 1 : 2;
  if (!copy_plane(
          u, f.width / 2, img->planes[u_index], img->stride[u_index], f.width / 2, f.height / 2) ||
      !copy_plane(
          v, f.width / 2, img->planes[v_index], img->stride[v_index], f.width / 2, f.height / 2))
    return std::nullopt;
  return f;
}

} // namespace nyx
