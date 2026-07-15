#include "nyx/recovery_phrase.hpp"

#include "nyx/util.hpp"

extern "C" {
#include <crypto/sha2/sha256.h>
}

#include <array>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace nyx {
namespace {

#include "bip39_english.inc"

const std::unordered_map<std::string, int>& word_index() {
  static const std::unordered_map<std::string, int> idx = [] {
    std::unordered_map<std::string, int> m;
    m.reserve(2048);
    for (int i = 0; i < 2048; ++i) m.emplace(kBip39Words[i], i);
    return m;
  }();
  return idx;
}

std::string join_words(const std::vector<std::string>& words) {
  std::ostringstream ss;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (i) ss << ' ';
    ss << words[i];
  }
  return ss.str();
}

std::vector<bool> bytes_to_bits(const uint8_t* data, std::size_t len) {
  std::vector<bool> bits;
  bits.reserve(len * 8);
  for (std::size_t i = 0; i < len; ++i) {
    for (int b = 7; b >= 0; --b) bits.push_back(((data[i] >> b) & 1) != 0);
  }
  return bits;
}

std::array<uint8_t, 16> bits_to_entropy(const std::vector<bool>& bits) {
  std::array<uint8_t, 16> entropy{};
  for (int i = 0; i < 128; ++i) {
    if (bits[static_cast<std::size_t>(i)]) {
      entropy[static_cast<std::size_t>(i / 8)] =
          static_cast<uint8_t>(entropy[static_cast<std::size_t>(i / 8)] |
                               (1u << (7 - (i % 8))));
    }
  }
  return entropy;
}

std::vector<int> bits_to_indices(const std::vector<bool>& bits) {
  std::vector<int> out;
  out.reserve(bits.size() / 11);
  for (std::size_t i = 0; i + 11 <= bits.size(); i += 11) {
    int v = 0;
    for (int b = 0; b < 11; ++b) v = (v << 1) | (bits[i + static_cast<std::size_t>(b)] ? 1 : 0);
    out.push_back(v);
  }
  return out;
}

bool phrase_checksum_ok(const std::vector<int>& indices) {
  if (indices.size() != 12) return false;
  std::vector<bool> bits;
  bits.reserve(132);
  for (int idx : indices) {
    for (int b = 10; b >= 0; --b) bits.push_back(((idx >> b) & 1) != 0);
  }
  const auto entropy = bits_to_entropy(bits);
  std::array<uint8_t, 32> hash{};
  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update(&ctx, entropy.data(), entropy.size());
  sha256_finish(&ctx, hash.data());
  for (int i = 0; i < 4; ++i) {
    const bool expected = ((hash[0] >> (7 - i)) & 1) != 0;
    if (bits[128 + static_cast<std::size_t>(i)] != expected) return false;
  }
  return true;
}

}  // namespace

std::string generate_recovery_phrase() {
  std::array<uint8_t, 16> entropy{};
  random_bytes(entropy.data(), entropy.size());

  std::array<uint8_t, 32> hash{};
  sha256_context_t ctx;
  sha256_reset(&ctx);
  sha256_update(&ctx, entropy.data(), entropy.size());
  sha256_finish(&ctx, hash.data());

  auto bits = bytes_to_bits(entropy.data(), entropy.size());
  for (int i = 0; i < 4; ++i) bits.push_back(((hash[0] >> (7 - i)) & 1) != 0);

  const auto indices = bits_to_indices(bits);
  std::vector<std::string> words;
  words.reserve(12);
  for (int idx : indices) words.emplace_back(kBip39Words[idx]);
  return join_words(words);
}

std::vector<std::string> split_recovery_words(const std::string& normalized_phrase) {
  std::vector<std::string> words;
  std::istringstream ss(normalized_phrase);
  std::string w;
  while (ss >> w) words.push_back(w);
  return words;
}

bool normalize_recovery_phrase(const std::string& phrase, std::string* normalized_out,
                               std::string* err) {
  std::string lowered;
  lowered.reserve(phrase.size());
  for (unsigned char c : phrase) {
    if (std::isspace(c)) {
      if (!lowered.empty() && lowered.back() != ' ') lowered.push_back(' ');
    } else {
      lowered.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  while (!lowered.empty() && lowered.back() == ' ') lowered.pop_back();

  const auto words = split_recovery_words(lowered);
  if (words.size() != 12) {
    if (err) *err = "нужно ровно 12 слов recovery-фразы";
    return false;
  }

  const auto& idx = word_index();
  std::vector<int> indices;
  indices.reserve(12);
  for (const auto& w : words) {
    const auto it = idx.find(w);
    if (it == idx.end()) {
      if (err) *err = "неизвестное слово: " + w;
      return false;
    }
    indices.push_back(it->second);
  }
  if (!phrase_checksum_ok(indices)) {
    if (err) *err = "фраза повреждена (неверный checksum)";
    return false;
  }
  if (normalized_out) *normalized_out = join_words(words);
  return true;
}

}  // namespace nyx
