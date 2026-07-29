#pragma once

#include "nyx/types.hpp"

#include <array>
#include <string>

namespace nyx {

using FileHash = std::array<uint8_t, 32>;

bool hash_file(const std::string& path, FileHash& out);

FileHash hash_bytes(const uint8_t* data, std::size_t len);

std::string hash_hex(const FileHash& hash);

bool hash_from_hex(const std::string& hex, FileHash& out);

}
