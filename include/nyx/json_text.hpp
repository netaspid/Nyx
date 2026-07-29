#pragma once

// Flat JSON store helpers. Escape/unescape must stay compatible with on-disk data.

#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nyx {

inline constexpr std::size_t kMaxJsonStoreBytes = 32u * 1024u * 1024u;

inline bool json_store_within_limit(std::size_t bytes) {
  return bytes <= kMaxJsonStoreBytes;
}

inline std::optional<std::string> json_read_file_limited(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::string {};
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  if (end < 0)
    return std::nullopt;
  const auto size = static_cast<std::size_t>(end);
  if (!json_store_within_limit(size))
    return std::nullopt;
  in.seekg(0, std::ios::beg);
  std::string out(size, '\0');
  if (size > 0)
    in.read(out.data(), static_cast<std::streamsize>(size));
  if (!in)
    return std::nullopt;
  return out;
}

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

inline std::vector<std::string> json_split_objects(const std::string& arr) {
  std::vector<std::string> out;
  std::size_t depth = 0;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const char c = arr[i];
    if (c == '{') {
      if (depth++ == 0)
        start = i;
    } else if (c == '}') {
      if (--depth == 0 && start != std::string::npos) {
        out.push_back(arr.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return out;
}

inline std::optional<std::pair<std::size_t, std::size_t>> json_array_bounds(const std::string& json,
                                                                            std::size_t from) {
  const auto start = json.find('[', from);
  if (start == std::string::npos)
    return std::nullopt;
  int depth = 0;
  for (std::size_t i = start; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '[')
      ++depth;
    else if (c == ']') {
      --depth;
      if (depth == 0)
        return std::make_pair(start, i);
    }
  }
  return std::nullopt;
}

inline void json_parse_object_array(const std::string& obj,
                                    const char* key,
                                    const std::function<void(const std::string&)>& on_object) {
  const std::string needle = std::string("\"") + key + "\":";
  const auto key_pos = obj.find(needle);
  if (key_pos == std::string::npos)
    return;
  const auto bounds = json_array_bounds(obj, key_pos + needle.size());
  if (!bounds)
    return;
  const auto [as, ae] = *bounds;
  for (const auto& item : json_split_objects(obj.substr(as, ae - as + 1)))
    on_object(item);
}

} // namespace nyx
