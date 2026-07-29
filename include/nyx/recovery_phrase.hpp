#pragma once

/** @file recovery_phrase.hpp
 *  Local recovery phrase (12 words, BIP39 English) for account password reset.
 */

#include <string>
#include <vector>

namespace nyx {

/** Generates a 12-word phrase (128 bits of entropy + BIP39 checksum bits). */
std::string generate_recovery_phrase();

/** Normalizes whitespace/case and validates words + checksum.
 *  @param phrase user input
 *  @param normalized_out canonical string (single-space separated) on success
 *  @return false on an unknown word or bad checksum
 */
bool normalize_recovery_phrase(const std::string& phrase, std::string* normalized_out,
                               std::string* err = nullptr);

/** Splits a normalized phrase into words. */
std::vector<std::string> split_recovery_words(const std::string& normalized_phrase);

}  // namespace nyx
