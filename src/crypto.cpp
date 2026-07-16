#include "nyx/crypto.hpp"

#include "nyx/util.hpp"

#include <noise/protocol.h>
#include <noise/protocol/handshakestate.h>
#include <crypto/sha2/sha256.h>

#include <cstring>

namespace nyx {

namespace {

std::uint64_t g_rekey_byte_limit = 0;

using NoiseHS = ::NoiseHandshakeState;
using NoiseCS = ::NoiseCipherState;

void destroy_hs(NoiseHS* p) {
  if (p) noise_handshakestate_free(p);
}

void destroy_cs(NoiseCS* p) {
  if (p) noise_cipherstate_free(p);
}

void derive_session_key(const std::array<uint8_t, 32>& binding, std::uint64_t epoch,
                        const char* label, uint8_t* out_key) {
  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update(&ctx, binding.data(), binding.size());
  uint8_t epoch_bytes[8];
  for (int i = 0; i < 8; ++i) {
    epoch_bytes[i] = static_cast<uint8_t>((epoch >> (8 * i)) & 0xff);
  }
  sha256_update(&ctx, epoch_bytes, sizeof(epoch_bytes));
  sha256_update(&ctx, reinterpret_cast<const uint8_t*>(label), std::strlen(label));
  sha256_finish(&ctx, out_key);
}

bool init_cipher_key(void* cipher, const uint8_t* key, std::size_t key_len) {
  return noise_cipherstate_init_key(static_cast<NoiseCS*>(cipher), key, key_len) ==
         NOISE_ERROR_NONE;
}

}  // namespace

void set_session_rekey_byte_limit(std::uint64_t bytes) { g_rekey_byte_limit = bytes; }

HandshakeDriver::HandshakeDriver(HandshakeRole role) : role_(role) {
  NoiseHS* hs = nullptr;
  const int err = noise_handshakestate_new_by_name(
      &hs, "Noise_XX_25519_ChaChaPoly_BLAKE2s",
      role == HandshakeRole::Initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER);
  if (err != NOISE_ERROR_NONE) return;

  static const uint8_t kPrologue[] = {'N', 'Y', 'X', 'v', '1'};
  noise_handshakestate_set_prologue(hs, kPrologue, sizeof(kPrologue));
  if (noise_handshakestate_needs_local_keypair(hs)) {
    NoiseDHState* dh = noise_handshakestate_get_local_keypair_dh(hs);
    noise_dhstate_generate_keypair(dh);
  }
  noise_handshakestate_start(hs);
  hs_ = hs;
}

HandshakeDriver::~HandshakeDriver() { destroy_hs(static_cast<NoiseHS*>(hs_)); }

std::optional<ByteBuffer> HandshakeDriver::step(const ByteBuffer* inbound) {
  auto* hs = static_cast<NoiseHS*>(hs_);
  if (!hs || complete_) return std::nullopt;

  if (inbound && !inbound->empty()) {
    uint8_t payload[256];
    NoiseBuffer inbuf;
    noise_buffer_set_input(inbuf, const_cast<uint8_t*>(inbound->data()), inbound->size());
    NoiseBuffer outbuf;
    noise_buffer_set_output(outbuf, payload, sizeof(payload));
    if (noise_handshakestate_read_message(hs, &inbuf, &outbuf) != NOISE_ERROR_NONE) {
      return std::nullopt;
    }
  }

  if (noise_handshakestate_get_action(hs) == NOISE_ACTION_SPLIT) {
    complete_ = true;
    return std::nullopt;
  }

  if (noise_handshakestate_get_action(hs) != NOISE_ACTION_WRITE_MESSAGE) {
    return std::nullopt;
  }

  uint8_t message[65535];
  NoiseBuffer buf;
  noise_buffer_set_output(buf, message, sizeof(message));
  const int err = noise_handshakestate_write_message(hs, &buf, nullptr);
  if (err != NOISE_ERROR_NONE || buf.size == 0) {
    return std::nullopt;
  }
  if (noise_handshakestate_get_action(hs) == NOISE_ACTION_SPLIT) complete_ = true;
  return ByteBuffer(message, message + buf.size);
}

Session::Session(void* send_cipher, void* recv_cipher, std::array<uint8_t, 32> binding_hash,
                 HandshakeRole role)
    : send_(send_cipher),
      recv_(recv_cipher),
      binding_hash_(binding_hash),
      role_(role),
      started_at_(std::chrono::steady_clock::now()) {}

Session::~Session() {
  destroy_cs(static_cast<NoiseCS*>(send_));
  destroy_cs(static_cast<NoiseCS*>(recv_));
}

Session::Session(Session&& other) noexcept
    : send_(other.send_),
      recv_(other.recv_),
      binding_hash_(other.binding_hash_),
      role_(other.role_),
      rekey_epoch_(other.rekey_epoch_),
      bytes_transferred_(other.bytes_transferred_),
      started_at_(other.started_at_) {
  other.send_ = nullptr;
  other.recv_ = nullptr;
}

Session& Session::operator=(Session&& other) noexcept {
  if (this != &other) {
    destroy_cs(static_cast<NoiseCS*>(send_));
    destroy_cs(static_cast<NoiseCS*>(recv_));
    send_ = other.send_;
    recv_ = other.recv_;
    binding_hash_ = other.binding_hash_;
    role_ = other.role_;
    rekey_epoch_ = other.rekey_epoch_;
    bytes_transferred_ = other.bytes_transferred_;
    started_at_ = other.started_at_;
    other.send_ = nullptr;
    other.recv_ = nullptr;
  }
  return *this;
}

std::optional<Session> Session::from_handshake(HandshakeDriver& hs) {
  if (!hs.complete_ || !hs.hs_) return std::nullopt;
  auto* h = static_cast<NoiseHS*>(hs.hs_);

  std::array<uint8_t, 32> binding{};
  if (noise_handshakestate_get_handshake_hash(h, binding.data(), binding.size()) !=
      NOISE_ERROR_NONE) {
    return std::nullopt;
  }

  NoiseCS* send = nullptr;
  NoiseCS* recv = nullptr;
  if (noise_handshakestate_split(h, &send, &recv) != NOISE_ERROR_NONE) {
    return std::nullopt;
  }
  return Session(send, recv, binding, hs.role());
}

void Session::note_transfer(std::size_t bytes) { bytes_transferred_ += bytes; }

bool Session::needs_rekey() const {
  const std::uint64_t limit =
      g_rekey_byte_limit > 0 ? g_rekey_byte_limit : kSessionRekeyBytes;
  if (bytes_transferred_ >= limit) return true;
  const auto now = std::chrono::steady_clock::now();
  return now - started_at_ >= kSessionRekeyMaxAge;
}

bool Session::perform_rekey(std::uint64_t epoch) {
  if (epoch <= rekey_epoch_) return true;

  uint8_t send_key[32];
  uint8_t recv_key[32];
  if (role_ == HandshakeRole::Initiator) {
    derive_session_key(binding_hash_, epoch, "nyx-tx", send_key);
    derive_session_key(binding_hash_, epoch, "nyx-rx", recv_key);
  } else {
    derive_session_key(binding_hash_, epoch, "nyx-rx", send_key);
    derive_session_key(binding_hash_, epoch, "nyx-tx", recv_key);
  }

  if (!init_cipher_key(send_, send_key, sizeof(send_key))) return false;
  if (!init_cipher_key(recv_, recv_key, sizeof(recv_key))) return false;

  rekey_epoch_ = epoch;
  bytes_transferred_ = 0;
  started_at_ = std::chrono::steady_clock::now();
  return true;
}

std::optional<ByteBuffer> Session::encrypt(const ByteBuffer& plain, std::string* err) {
  auto* cs = static_cast<NoiseCS*>(send_);
  if (!cs) {
    if (err) *err = "no send cipher";
    return std::nullopt;
  }
  // Noise cipherstate: max plaintext 65535 - 16 (MAC). Больший буфер → порча кучи в noise-c.
  constexpr std::size_t kNoiseMaxPlain = 65535 - 16;
  if (plain.size() > kNoiseMaxPlain) {
    if (err) *err = "plaintext too large for Noise";
    return std::nullopt;
  }
  ByteBuffer out(plain.size() + 16);
  NoiseBuffer buf;
  noise_buffer_set_inout(buf, out.data(), plain.size(), out.size());
  std::memcpy(buf.data, plain.data(), plain.size());
  if (noise_cipherstate_encrypt(cs, &buf) != NOISE_ERROR_NONE) {
    if (err) *err = "encrypt failed";
    return std::nullopt;
  }
  out.resize(buf.size);
  note_transfer(out.size());
  return out;
}

std::optional<ByteBuffer> Session::decrypt(const ByteBuffer& cipher, std::string* err) {
  auto* cs = static_cast<NoiseCS*>(recv_);
  if (!cs) {
    if (err) *err = "no recv cipher";
    return std::nullopt;
  }
  constexpr std::size_t kNoiseMaxCipher = 65535;
  if (cipher.size() > kNoiseMaxCipher) {
    if (err) *err = "ciphertext too large for Noise";
    return std::nullopt;
  }
  ByteBuffer out(cipher.size());
  NoiseBuffer buf;
  noise_buffer_set_inout(buf, out.data(), cipher.size(), out.size());
  std::memcpy(buf.data, cipher.data(), cipher.size());
  if (noise_cipherstate_decrypt(cs, &buf) != NOISE_ERROR_NONE) {
    if (err) *err = "decrypt failed";
    return std::nullopt;
  }
  out.resize(buf.size);
  note_transfer(cipher.size());
  return out;
}

}  // namespace nyx
