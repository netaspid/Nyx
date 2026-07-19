#pragma once

/** @file markdown_format.hpp
 *  Nyx Markdown → HTML / блоки для пузырей (RichText + медиа).
 */

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace nyx {

/** Тип блока сообщения. */
enum class MdBlockType : uint8_t {
  Paragraph = 0,
  Table = 1,
  Formula = 2,
  Media = 3,
  Action = 4,
};

/** Один блок после разбора текста сообщения. */
struct MdBlock {
  MdBlockType type = MdBlockType::Paragraph;
  /** paragraph / table raw / formula latex / action body. */
  std::string text;
  /** Медиа: hex hash. */
  std::string hash;
  /** Медиа: подпись. */
  std::string caption;
  /** $$…$$ vs $…$. */
  bool display_math = false;
};

/** Экранирование HTML-сущностей. */
std::string html_escape(const std::string& s);

/** `/me foo` → `nyx-me:foo`; иначе исходная строка (trim справа не трогаем). */
std::string normalize_me_message(const std::string& text);

/** Сообщение-действие. */
bool is_action_message(const std::string& text);

/** Тело после `nyx-me:`. */
std::string action_message_body(const std::string& text);

/** Разбор на блоки: action / formula / media / table / paragraph. */
std::vector<MdBlock> parse_markdown_blocks(const std::string& src);

/** Lite TeX → HTML (греческий, frac, sqrt, ^ _). */
std::string formula_to_html(const std::string& latex);

/** GFM pipe-table → HTML table. */
std::string table_to_html(const std::string& table_src);

/**
 * Markdown → HTML для paragraph-блока.
 * fence, code, spoiler, links (http + nyx-user:), **bold**, __u__, ~~s~~, *i*,
 * > quote, # headings, lists, ---, $inline math$.
 */
std::string markdown_to_html(const std::string& src,
                             const std::set<int>& revealed_spoilers = {});

}  // namespace nyx
