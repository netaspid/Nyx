#pragma once

/** @file markdown_format.hpp
 *  Nyx Markdown -> HTML / bubble blocks (RichText + media).
 */

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace nyx {

/** Message block type. */
enum class MdBlockType : uint8_t {
  Paragraph = 0,
  Table = 1,
  Formula = 2,
  Media = 3,
  Action = 4,
  File = 5,
};

/** One block after parsing a message text. */
struct MdBlock {
  MdBlockType type = MdBlockType::Paragraph;
  /** paragraph / table raw / formula latex / action body. */
  std::string text;
  /** Media: hex hash. */
  std::string hash;
  /** Media: caption. */
  std::string caption;
  /** File-card MIME and size. */
  std::string mime;
  uint64_t size = 0;
  /** $$…$$ vs $…$. */
  bool display_math = false;
};

/** Escapes HTML entities. */
std::string html_escape(const std::string& s);

/** `/me foo` -> `nyx-me:foo`; otherwise the input string (no right trim). */
std::string normalize_me_message(const std::string& text);

/** Action message. */
bool is_action_message(const std::string& text);

/** Body after `nyx-me:`. */
std::string action_message_body(const std::string& text);

/** Splits into blocks: action / formula / media / table / paragraph. */
std::vector<MdBlock> parse_markdown_blocks(const std::string& src);

/** Lite TeX -> HTML (Greek letters, frac, sqrt, ^ _). */
std::string formula_to_html(const std::string& latex);

/** GFM pipe-table → HTML table. */
std::string table_to_html(const std::string& table_src);

/**
 * Markdown -> HTML for a paragraph block.
 * fence, code, spoiler, links (http + nyx-user:), **bold**, __u__, ~~s~~, *i*,
 * > quote, # headings, lists, ---, $inline math$.
 */
std::string markdown_to_html(const std::string& src, const std::set<int>& revealed_spoilers = {});

} // namespace nyx
