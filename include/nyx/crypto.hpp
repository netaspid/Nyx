#pragma once

/** @file crypto.hpp
 *  Noise XX handshake и шифрованная сессия (ChaCha20-Poly1305).
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
constexpr std::chrono::hours kSessionRekeyMaxAge{24};

/** Для тестов: 0 = порог по умолчанию (1 GB). */
void set_session_rekey_byte_limit(std::uint64_t bytes);

/** Драйвер пошагового Noise handshake (один вызов step = read и/или write). */
class HandshakeDriver {
 public:
  explicit HandshakeDriver(HandshakeRole role);
  ~HandshakeDriver();

  HandshakeDriver(const HandshakeDriver&) = delete;
  HandshakeDriver& operator=(const HandshakeDriver&) = delete;

  bool complete() const { return complete_; }
  HandshakeRole role() const { return role_; }

  /** @param inbound кадр handshake от peer или nullptr для первого/исходящего шага.
   *  @return байты исходящего сообщения или nullopt если ждём ввод/split. */
  std::optional<ByteBuffer> step(const ByteBuffer* inbound = nullptr);

 private:
  friend class Session;
  HandshakeRole role_;
  void* hs_ = nullptr;
  bool complete_ = false;
};

/** Симметричное шифрование после успешного handshake. */
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

  /** Текущий epoch rekey (0 после handshake). */
  std::uint64_t rekey_epoch() const { return rekey_epoch_; }

  /** Суммарный объём шифротекста с последнего rekey. */
  std::uint64_t bytes_transferred() const { return bytes_transferred_; }

  /** Нужна ротация по protocol.md (1 GB / 24 h). */
  bool needs_rekey() const;

  /** Детерминированная ротация ключей; epoch должен только расти. */
  bool perform_rekey(std::uint64_t epoch);

 private:
  Session(void* send_cipher, void* recv_cipher, std::array<uint8_t, 32> binding_hash,
          HandshakeRole role);

  void note_transfer(std::size_t bytes);

  void* send_ = nullptr;
  void* recv_ = nullptr;
  std::array<uint8_t, 32> binding_hash_{};
  HandshakeRole role_ = HandshakeRole::Initiator;
  std::uint64_t rekey_epoch_ = 0;
  std::uint64_t bytes_transferred_ = 0;
  std::chrono::steady_clock::time_point started_at_{};
};

}  // namespace nyx
