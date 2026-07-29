#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace nyx {

enum class MdBlockType : uint8_t {
  Paragraph = 0,
  Table = 1,
  Formula = 2,
  Media = 3,
  Action = 4,
  File = 5,
};

struct MdBlock {
  MdBlockType type = MdBlockType::Paragraph;

  std::string text;

  std::string hash;

  std::string caption;

  std::string mime;
  uint64_t size = 0;

  bool display_math = false;
};

std::string html_escape(const std::string& s);

std::string normalize_me_message(const std::string& text);

bool is_action_message(const std::string& text);

std::string action_message_body(const std::string& text);

std::vector<MdBlock> parse_markdown_blocks(const std::string& src);

std::string formula_to_html(const std::string& latex);

std::string table_to_html(const std::string& table_src);

std::string markdown_to_html(const std::string& src, const std::set<int>& revealed_spoilers = {});

}
