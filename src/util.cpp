#include "nyx/util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace nyx {

namespace {}

std::string to_hex(const uint8_t* data, std::size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kHex[data[i] >> 4]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
  if (hex.size() % 2 != 0)
    return false;
  out.clear();
  out.reserve(hex.size() / 2);
  auto nybble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    int hi = nybble(hex[i]);
    int lo = nybble(hex[i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

void write_u16_le(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void write_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void write_u64_le(std::vector<uint8_t>& buf, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

uint64_t read_u64_le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

void random_bytes(uint8_t* out, std::size_t len) {
  static thread_local std::mt19937 rng {std::random_device {}()};
  std::uniform_int_distribution<int> dist(0, 255);
  for (std::size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(dist(rng));
  }
}

bool parse_host_port(const std::string& addr, std::string& host, uint16_t& port) {
  const auto pos = addr.rfind(':');
  if (pos == std::string::npos)
    return false;
  host = addr.substr(0, pos);
  try {
    port = static_cast<uint16_t>(std::stoi(addr.substr(pos + 1)));
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

bool endpoint_matches(const std::string& from_host,
                      uint16_t from_port,
                      const std::string& expected_host,
                      uint16_t expected_port) {
  if (from_port != expected_port)
    return false;
  if (from_host == expected_host)
    return true;

  in_addr from_addr {};
  if (inet_pton(AF_INET, from_host.c_str(), &from_addr) != 1)
    return false;

  addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(expected_port);
  if (getaddrinfo(expected_host.c_str(), port_str.c_str(), &hints, &res) != 0) {
    return false;
  }

  bool match = false;
  for (auto* p = res; p != nullptr; p = p->ai_next) {
    if (p->ai_family != AF_INET)
      continue;
    const auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
    if (sin->sin_addr.s_addr == from_addr.s_addr) {
      match = true;
      break;
    }
  }
  freeaddrinfo(res);
  return match;
}

std::filesystem::path path_from_utf8(const std::string& utf8) {
  if (utf8.empty())
    return {};
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen <= 0)
    return std::filesystem::path(utf8);
  std::wstring wide(static_cast<std::size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wlen);
  return std::filesystem::path(wide);
#else
  return std::filesystem::path(utf8);
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::wstring wide = path.native();
  if (wide.empty())
    return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0)
    return path.string();
  std::string utf8(static_cast<std::size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), len, nullptr, nullptr);
  return utf8;
#else
  return path.string();
#endif
}

std::string normalize_utf8_path(const std::string& utf8) {
  return path_to_utf8(path_from_utf8(utf8).lexically_normal());
}

std::string normalize_grant_root(const std::string& root_path) {
  std::string out = normalize_utf8_path(root_path);
#ifdef _WIN32
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
  return out;
}

}
