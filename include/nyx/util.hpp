#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nyx {

std::string to_hex(const uint8_t* data, std::size_t len);

bool from_hex(const std::string& hex, std::vector<uint8_t>& out);

void write_u16_le(std::vector<uint8_t>& buf, uint16_t v);
void write_u32_le(std::vector<uint8_t>& buf, uint32_t v);
void write_u64_le(std::vector<uint8_t>& buf, uint64_t v);
uint16_t read_u16_le(const uint8_t* p);
uint32_t read_u32_le(const uint8_t* p);
uint64_t read_u64_le(const uint8_t* p);

void random_bytes(uint8_t* out, std::size_t len);

bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port);

bool endpoint_matches(const std::string& from_host,
                      uint16_t from_port,
                      const std::string& expected_host,
                      uint16_t expected_port);

std::filesystem::path path_from_utf8(const std::string& utf8);

std::string path_to_utf8(const std::filesystem::path& path);

std::string normalize_utf8_path(const std::string& utf8);

std::string normalize_grant_root(const std::string& root_path);

} // namespace nyx
