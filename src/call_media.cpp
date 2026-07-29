#include "nyx/call_media.hpp"

#include "nyx/util.hpp"

#include <algorithm>

namespace nyx {

ByteBuffer CallMediaFrame::encode() const {
  ByteBuffer out;
  const bool extended = origin != UserId {};
  out.reserve(1 + 4 + (extended ? kPublicKeySize + 2 : 0) + payload.size());
  out.push_back(static_cast<uint8_t>(type) | (extended ? 0x80 : 0));
  write_u32_le(out, seq);
  if (extended) {
    out.insert(out.end(), origin.begin(), origin.end());
    out.push_back(hop_count);
    out.push_back(audio_level);
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::optional<CallMediaFrame> CallMediaFrame::decode(const ByteBuffer& data) {
  if (data.size() < 5)
    return std::nullopt;
  CallMediaFrame f;
  const bool extended = (data[0] & 0x80) != 0;
  f.type = static_cast<CallMediaType>(data[0] & 0x7f);
  if (f.type != CallMediaType::Opus && f.type != CallMediaType::Video &&
      f.type != CallMediaType::Fec && f.type != CallMediaType::Nack)
    return std::nullopt;
  f.seq = read_u32_le(data.data() + 1);
  std::size_t offset = 5;
  if (extended) {
    if (data.size() < offset + kPublicKeySize + 2)
      return std::nullopt;
    std::copy_n(data.data() + offset, kPublicKeySize, f.origin.begin());
    offset += kPublicKeySize;
    f.hop_count = data[offset++];
    f.audio_level = data[offset++];
  }
  f.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(offset), data.end());
  if (f.payload.size() > kMaxCallMediaPayload)
    return std::nullopt;
  return f;
}

} // namespace nyx
