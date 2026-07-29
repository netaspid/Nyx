#pragma once

/** @file file_hash.hpp
 *  SHA-256 hashing of files and buffers.
 */

#include "nyx/types.hpp"

#include <array>
#include <string>

namespace nyx {

using FileHash = std::array<uint8_t, 32>;

/** SHA-256 of file contents. @return false when the file is unreadable. */
bool hash_file(const std::string& path, FileHash& out);

/** SHA-256 of a byte buffer. */
FileHash hash_bytes(const uint8_t* data, std::size_t len);

/** Hex form of the hash (64 chars). */
std::string hash_hex(const FileHash& hash);

/** Parses hex into a FileHash. */
bool hash_from_hex(const std::string& hex, FileHash& out);

}  // namespace nyx
