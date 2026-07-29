#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nyx_web {

inline std::string json_escape(std::string_view s) {
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
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

inline std::string json_str(std::string_view s) {
  return "\"" + json_escape(s) + "\"";
}

inline std::optional<std::string> json_get_string(std::string_view json, std::string_view key) {
  const std::string pat = "\"" + std::string(key) + "\":\"";
  auto pos = json.find(pat);
  if (pos == std::string_view::npos)
    return std::nullopt;
  pos += pat.size();
  std::string out;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '\\' && pos < json.size()) {
      char n = json[pos++];
      if (n == 'n')
        out += '\n';
      else if (n == 'r')
        out += '\r';
      else if (n == 't')
        out += '\t';
      else
        out += n;
      continue;
    }
    if (c == '"')
      break;
    out += c;
  }
  return out;
}

inline std::optional<bool> json_get_bool(std::string_view json, std::string_view key) {
  const std::string pat = "\"" + std::string(key) + "\":";
  auto pos = json.find(pat);
  if (pos == std::string_view::npos)
    return std::nullopt;
  pos += pat.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (json.substr(pos, 4) == "true")
    return true;
  if (json.substr(pos, 5) == "false")
    return false;
  return std::nullopt;
}

inline std::optional<int64_t> json_get_int(std::string_view json, std::string_view key) {
  const std::string pat = "\"" + std::string(key) + "\":";
  auto pos = json.find(pat);
  if (pos == std::string_view::npos)
    return std::nullopt;
  pos += pat.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  bool neg = false;
  if (pos < json.size() && json[pos] == '-') {
    neg = true;
    ++pos;
  }
  int64_t v = 0;
  bool any = false;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    any = true;
    v = v * 10 + (json[pos] - '0');
    ++pos;
  }
  if (!any)
    return std::nullopt;
  return neg ? -v : v;
}

inline std::string json_obj_get(std::string_view json, std::string_view key) {
  const std::string pat = "\"" + std::string(key) + "\":";
  auto pos = json.find(pat);
  if (pos == std::string_view::npos)
    return {};
  pos += pat.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (pos >= json.size())
    return {};
  if (json[pos] == '{') {
    int depth = 0;
    auto start = pos;
    for (; pos < json.size(); ++pos) {
      if (json[pos] == '{')
        ++depth;
      else if (json[pos] == '}') {
        --depth;
        if (depth == 0)
          return std::string(json.substr(start, pos - start + 1));
      }
    }
  }
  if (json[pos] == '"') {
    auto s = json_get_string(json, key);
    return s ? json_str(*s) : "{}";
  }
  auto end = json.find_first_of(",}", pos);
  if (end == std::string_view::npos)
    end = json.size();
  return std::string(json.substr(pos, end - pos));
}

} // namespace nyx_web
