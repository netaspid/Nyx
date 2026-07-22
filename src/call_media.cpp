#include "nyx/call_media.hpp"

#include "nyx/util.hpp"

namespace nyx {

ByteBuffer CallMediaFrame::encode() const {
  ByteBuffer out;
  out.reserve(1 + 4 + payload.size());
  out.push_back(static_cast<uint8_t>(type));
  write_u32_le(out, seq);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::optional<CallMediaFrame> CallMediaFrame::decode(const ByteBuffer& data) {
  if (data.size() < 5) return std::nullopt;
  CallMediaFrame f;
  f.type = static_cast<CallMediaType>(data[0]);
  if (f.type != CallMediaType::Opus && f.type != CallMediaType::Video &&
      f.type != CallMediaType::Fec && f.type != CallMediaType::Nack)
    return std::nullopt;
  f.seq = read_u32_le(data.data() + 1);
  f.payload.assign(data.begin() + 5, data.end());
  if (f.payload.size() > kMaxCallMediaPayload) return std::nullopt;
  return f;
}

}  // namespace nyx
