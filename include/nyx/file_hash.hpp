#pragma once

/** @file file_hash.hpp
 *  SHA-256 хеш файлов и буферов (фаза 4).
 */

#include "nyx/types.hpp"

#include <array>
#include <string>

namespace nyx {

using FileHash = std::array<uint8_t, 32>;

/** SHA-256 содержимого файла. @return false если файл не читается. */
bool hash_file(const std::string& path, FileHash& out);

/** SHA-256 байтового буфера. */
FileHash hash_bytes(const uint8_t* data, std::size_t len);

/** Hex-представление хеша (64 символа). */
std::string hash_hex(const FileHash& hash);

/** Парсинг hex в FileHash. */
bool hash_from_hex(const std::string& hex, FileHash& out);

}  // namespace nyx
