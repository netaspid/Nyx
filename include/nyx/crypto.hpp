#pragma once

/** @file crypto.hpp
 *  Noise XX handshake and encrypted session (ChaCha20-Poly1305).
 */

#include "nyx/types.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace nyx {

constexpr std::uint64_t kSessionRekeyBytes = 1024ULL * 1024 * 1024;
constexpr std::chrono::hours kSessionRekeyMaxAge {24};

/** For tests: 0 = default threshold (1 GB). */
void set_session_rekey_byte_limit(std::uint64_t bytes);

/** Stepwise Noise handshake driver (one step call = read and/or write). */
class HandshakeDriver {
public:
  explicit HandshakeDriver(HandshakeRole role);
  ~HandshakeDriver();

  HandshakeDriver(const HandshakeDriver&) = delete;
  HandshakeDriver& operator=(const HandshakeDriver&) = delete;

  bool complete() const { return complete_; }
  HandshakeRole role() const { return role_; }

  /** @param inbound handshake frame from the peer, or nullptr for the first/outgoing step.
   *  @return outgoing message bytes, or nullopt while waiting for input/split. */
  std::optional<ByteBuffer> step(const ByteBuffer* inbound = nullptr);

private:
  friend class Session;
  HandshakeRole role_;
  void* hs_ = nullptr;
  bool complete_ = false;
};

/** Symmetric encryption after a successful handshake. */
class Session {
public:
  static std::optional<Session> from_handshake(HandshakeDriver& hs);

  Session(Session&& other) noexcept;
  Session& operator=(Session&& other) noexcept;
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  std::optional<ByteBuffer> encrypt(const ByteBuffer& plain, std::string* err = nullptr);
  std::optional<ByteBuffer> decrypt(const ByteBuffer& cipher, std::string* err = nullptr);
  /** Stateless-per-packet realtime AEAD: nonce is the UDP frame sequence. */
  std::optional<ByteBuffer>
  encrypt_realtime(std::uint64_t nonce, const ByteBuffer& plain, std::string* err = nullptr);
  std::optional<ByteBuffer>
  decrypt_realtime(std::uint64_t nonce, const ByteBuffer& cipher, std::string* err = nullptr);

  /** Current rekey epoch (0 right after the handshake). */
  std::uint64_t rekey_epoch() const { return rekey_epoch_; }

  /** Total ciphertext volume since the last rekey. */
  std::uint64_t bytes_transferred() const { return bytes_transferred_; }

  /** Rotation required per protocol.md (1 GB / 24 h). */
  bool needs_rekey() const;

  /** Deterministic key rotation; the epoch must only grow. */
  bool perform_rekey(std::uint64_t epoch);

private:
  Session(void* send_cipher,
          void* recv_cipher,
          std::array<uint8_t, 32> binding_hash,
          HandshakeRole role);

  void note_transfer(std::size_t bytes);
  bool init_realtime_keys(std::uint64_t epoch);

  void* send_ = nullptr;
  void* recv_ = nullptr;
  std::array<uint8_t, 32> realtime_send_key_ {};
  std::array<uint8_t, 32> realtime_recv_key_ {};
  std::array<uint8_t, 32> binding_hash_ {};
  HandshakeRole role_ = HandshakeRole::Initiator;
  std::uint64_t rekey_epoch_ = 0;
  std::uint64_t bytes_transferred_ = 0;
  std::chrono::steady_clock::time_point started_at_ {};
};

} // namespace nyx
