#pragma once

// Minimal helpers for the flat JSON files written by the local stores.
// Escape/unescape rules must stay compatible with data already on disk.

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

namespace nyx {

inline std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

inline std::optional<std::string> json_get_string(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos)
    return std::nullopt;
  std::size_t i = pos + needle.size();
  std::string out;
  while (i < json.size()) {
    const char c = json[i++];
    if (c == '"')
      break;
    if (c == '\\' && i < json.size()) {
      const char esc = json[i++];
      if (esc == 'n')
        out.push_back('\n');
      else if (esc == 'r')
        out.push_back('\r');
      else if (esc == 't')
        out.push_back('\t');
      else
        out.push_back(esc);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

inline uint64_t json_get_u64(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos)
    return 0;
  try {
    return std::stoull(json.substr(pos + needle.size()));
  } catch (const std::exception&) {
    return 0;
  }
}

inline std::optional<uint32_t> json_get_uint(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos)
    return std::nullopt;
  std::size_t i = pos + needle.size();
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])))
    ++i;
  std::size_t j = i;
  while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j])))
    ++j;
  if (j == i)
    return std::nullopt;
  return static_cast<uint32_t>(std::stoul(json.substr(i, j - i)));
}

inline std::optional<bool> json_get_bool(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos)
    return std::nullopt;
  const auto sub = json.substr(pos + needle.size(), 8);
  if (sub.rfind("true", 0) == 0)
    return true;
  if (sub.rfind("false", 0) == 0)
    return false;
  return std::nullopt;
}

} // namespace nyx
