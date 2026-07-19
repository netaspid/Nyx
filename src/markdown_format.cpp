#include "nyx/markdown_format.hpp"

#include <cctype>
#include <map>
#include <regex>
#include <vector>

namespace nyx {

std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

static std::string trim_left(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

static std::string trim_copy(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool is_action_message(const std::string& text) {
  return text.rfind("nyx-me:", 0) == 0;
}

std::string action_message_body(const std::string& text) {
  if (!is_action_message(text)) return text;
  return text.substr(7);
}

std::string normalize_me_message(const std::string& text) {
  const std::string t = trim_copy(text);
  if (t.size() >= 4 && t.compare(0, 4, "/me ") == 0) {
    return std::string("nyx-me:") + trim_copy(t.substr(4));
  }
  if (t.size() >= 4 && t.compare(0, 4, "/me\t") == 0) {
    return std::string("nyx-me:") + trim_copy(t.substr(4));
  }
  return text;
}

static bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

static bool is_media_line(const std::string& line, std::string& caption, std::string& hash) {
  static const std::regex re(R"(^!\[([^\]]*)\]\(nyx-media:([a-fA-F0-9]{64})\)\s*$)");
  std::smatch m;
  if (!std::regex_match(line, m, re)) return false;
  caption = m[1].str();
  hash = m[2].str();
  return is_hex64(hash);
}

static bool is_table_sep_line(const std::string& line) {
  const std::string t = trim_copy(line);
  if (t.empty() || t.find('|') == std::string::npos) return false;
  for (char c : t) {
    if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
  }
  return t.find('-') != std::string::npos;
}

static bool looks_like_table_row(const std::string& line) {
  const std::string t = trim_copy(line);
  return t.size() >= 3 && t.find('|') != std::string::npos;
}

std::string formula_to_html(const std::string& latex) {
  static const std::map<std::string, std::string> greeks = {
      {"\\alpha", "α"},   {"\\beta", "β"},   {"\\gamma", "γ"}, {"\\delta", "δ"},
      {"\\epsilon", "ε"}, {"\\zeta", "ζ"},   {"\\eta", "η"},   {"\\theta", "θ"},
      {"\\lambda", "λ"},  {"\\mu", "μ"},     {"\\pi", "π"},    {"\\rho", "ρ"},
      {"\\sigma", "σ"},   {"\\tau", "τ"},    {"\\phi", "φ"},   {"\\omega", "ω"},
      {"\\Gamma", "Γ"},   {"\\Delta", "Δ"},  {"\\Theta", "Θ"}, {"\\Lambda", "Λ"},
      {"\\Pi", "Π"},      {"\\Sigma", "Σ"},  {"\\Omega", "Ω"}, {"\\infty", "∞"},
      {"\\pm", "±"},      {"\\times", "×"},  {"\\cdot", "·"},  {"\\leq", "≤"},
      {"\\geq", "≥"},     {"\\neq", "≠"},    {"\\approx", "≈"},
  };
  std::string plain = trim_copy(latex);
  for (const auto& kv : greeks) replace_all(plain, kv.first, kv.second);
  {
    static const std::regex re(R"(\\frac\{([^{}]+)\}\{([^{}]+)\})");
    plain = std::regex_replace(plain, re, "($1/$2)");
  }
  {
    static const std::regex re(R"(\\sqrt\{([^{}]+)\})");
    plain = std::regex_replace(plain, re, "√($1)");
  }
  plain = html_escape(plain);
  {
    static const std::regex re_sup(R"(\^\{([^}]+)\})");
    plain = std::regex_replace(plain, re_sup, "<sup>$1</sup>");
    static const std::regex re_sub(R"(_\{([^}]+)\})");
    plain = std::regex_replace(plain, re_sub, "<sub>$1</sub>");
    static const std::regex re_sup1(R"(\^([A-Za-z0-9]))");
    plain = std::regex_replace(plain, re_sup1, "<sup>$1</sup>");
    static const std::regex re_sub1(R"(_([A-Za-z0-9]))");
    plain = std::regex_replace(plain, re_sub1, "<sub>$1</sub>");
  }
  replace_all(plain, "\\", "");
  return "<span style=\"font-family:Consolas,'Segoe UI',monospace;font-style:italic;\">" +
         plain + "</span>";
}

std::string table_to_html(const std::string& table_src) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= table_src.size()) {
    const auto nl = table_src.find('\n', start);
    lines.push_back(
        table_src.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (lines.size() < 2) return html_escape(table_src);

  auto split_row = [](const std::string& line) {
    std::vector<std::string> cells;
    std::string t = trim_copy(line);
    if (!t.empty() && t.front() == '|') t.erase(t.begin());
    if (!t.empty() && t.back() == '|') t.pop_back();
    std::size_t p = 0;
    while (p <= t.size()) {
      const auto bar = t.find('|', p);
      cells.push_back(trim_copy(t.substr(p, bar == std::string::npos ? std::string::npos : bar - p)));
      if (bar == std::string::npos) break;
      p = bar + 1;
    }
    return cells;
  };

  std::string html =
      "<table style=\"border-collapse:collapse;margin:4px 0;font-size:13px;\">";
  bool header = true;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (is_table_sep_line(lines[i])) {
      header = false;
      continue;
    }
    if (!looks_like_table_row(lines[i])) continue;
    const auto cells = split_row(lines[i]);
    html += "<tr>";
    for (const auto& c : cells) {
      html += header ? "<th style=\"border:1px solid #888;padding:3px 6px;\">"
                     : "<td style=\"border:1px solid #888;padding:3px 6px;\">";
      html += html_escape(c);
      html += header ? "</th>" : "</td>";
    }
    html += "</tr>";
    if (header) header = false;
  }
  html += "</table>";
  return html;
}

std::vector<MdBlock> parse_markdown_blocks(const std::string& src) {
  std::vector<MdBlock> blocks;
  if (is_action_message(src)) {
    MdBlock b;
    b.type = MdBlockType::Action;
    b.text = action_message_body(src);
    blocks.push_back(std::move(b));
    return blocks;
  }

  std::vector<std::string> lines;
  {
    std::size_t start = 0;
    while (start <= src.size()) {
      const auto nl = src.find('\n', start);
      lines.push_back(src.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
  }

  auto flush_para = [&](std::string& acc) {
    if (acc.empty()) return;
    // trim trailing newlines only
    while (!acc.empty() && acc.back() == '\n') acc.pop_back();
    if (acc.empty()) return;
    MdBlock b;
    b.type = MdBlockType::Paragraph;
    b.text = acc;
    blocks.push_back(std::move(b));
    acc.clear();
  };

  std::string para;
  bool in_fence = false;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    const std::string trimmed = trim_left(line);

    if (trimmed.rfind("```", 0) == 0) {
      in_fence = !in_fence;
      if (!para.empty()) para.push_back('\n');
      para += line;
      continue;
    }
    if (in_fence) {
      if (!para.empty()) para.push_back('\n');
      para += line;
      continue;
    }

    // Display math $$...$$ on one line or opening $$
    if (trimmed.rfind("$$", 0) == 0) {
      flush_para(para);
      std::string body = trimmed.substr(2);
      bool closed = false;
      if (body.size() >= 2 && body.size() >= 2) {
        const auto end = body.rfind("$$");
        if (end != std::string::npos && end > 0) {
          MdBlock b;
          b.type = MdBlockType::Formula;
          b.display_math = true;
          b.text = trim_copy(body.substr(0, end));
          blocks.push_back(std::move(b));
          closed = true;
        } else if (body.empty() || (body.size() >= 2 && body.substr(body.size() - 2) == "$$")) {
          if (body.size() >= 2 && body.substr(body.size() - 2) == "$$") {
            MdBlock b;
            b.type = MdBlockType::Formula;
            b.display_math = true;
            b.text = trim_copy(body.substr(0, body.size() - 2));
            blocks.push_back(std::move(b));
            closed = true;
          }
        }
      }
      if (!closed) {
        std::string math = body;
        ++i;
        for (; i < lines.size(); ++i) {
          const std::string t2 = trim_copy(lines[i]);
          if (t2 == "$$" || (t2.size() >= 2 && t2.substr(t2.size() - 2) == "$$")) {
            if (t2 != "$$") {
              if (!math.empty()) math.push_back('\n');
              math += t2.substr(0, t2.size() - 2);
            }
            break;
          }
          if (!math.empty()) math.push_back('\n');
          math += lines[i];
        }
        MdBlock b;
        b.type = MdBlockType::Formula;
        b.display_math = true;
        b.text = trim_copy(math);
        blocks.push_back(std::move(b));
      }
      continue;
    }

    std::string cap, hash;
    if (is_media_line(trim_copy(line), cap, hash)) {
      flush_para(para);
      MdBlock b;
      b.type = MdBlockType::Media;
      b.hash = hash;
      b.caption = cap;
      blocks.push_back(std::move(b));
      continue;
    }

    // Table: row + separator
    if (looks_like_table_row(line) && i + 1 < lines.size() && is_table_sep_line(lines[i + 1])) {
      flush_para(para);
      std::string table = line;
      ++i;
      table.push_back('\n');
      table += lines[i];
      ++i;
      for (; i < lines.size() && looks_like_table_row(lines[i]) && !is_table_sep_line(lines[i]);
           ++i) {
        table.push_back('\n');
        table += lines[i];
      }
      --i;
      MdBlock b;
      b.type = MdBlockType::Table;
      b.text = table;
      blocks.push_back(std::move(b));
      continue;
    }

    if (!para.empty()) para.push_back('\n');
    para += line;
  }
  flush_para(para);
  if (blocks.empty() && !src.empty()) {
    MdBlock b;
    b.type = MdBlockType::Paragraph;
    b.text = src;
    blocks.push_back(std::move(b));
  }
  return blocks;
}

std::string markdown_to_html(const std::string& src, const std::set<int>& revealed_spoilers) {
  std::string text = src;

  // Protect display/inline math and fences
  std::vector<std::string> fences;
  {
    static const std::regex re(R"(```([A-Za-z0-9_+#.-]*)[ \t]*\n?([\s\S]*?)```)");
    std::smatch m;
    std::string acc;
    auto it = text.cbegin();
    while (std::regex_search(it, text.cend(), m, re)) {
      acc.append(it, m[0].first);
      const std::string lang = m[1].str();
      std::string code = m[2].str();
      if (!code.empty() && code.front() == '\n') code.erase(code.begin());
      if (!code.empty() && code.back() == '\n') code.pop_back();
      const auto idx = fences.size();
      std::string html;
      if (!lang.empty())
        html += "<div style=\"font-size:11px;opacity:0.7;margin-bottom:2px;\">" + html_escape(lang) +
                "</div>";
      html +=
          "<pre style=\"white-space:pre-wrap;font-family:Consolas,monospace;font-size:13px;\">" +
          html_escape(code) + "</pre>";
      fences.push_back(std::move(html));
      acc += std::string("\x01") + "F" + std::to_string(idx) + "\x01";
      it = m[0].second;
    }
    acc.append(it, text.cend());
    text = std::move(acc);
  }

  std::vector<std::string> maths;
  {
    static const std::regex re(R"(\$([^\$\n]+)\$)");
    std::smatch m;
    std::string acc;
    auto it = text.cbegin();
    while (std::regex_search(it, text.cend(), m, re)) {
      acc.append(it, m[0].first);
      const auto idx = maths.size();
      maths.push_back(formula_to_html(m[1].str()));
      acc += std::string("\x01") + "M" + std::to_string(idx) + "\x01";
      it = m[0].second;
    }
    acc.append(it, text.cend());
    text = std::move(acc);
  }

  std::vector<std::string> inlines;
  {
    static const std::regex re(R"(`([^`\n]+)`)");
    std::smatch m;
    std::string acc;
    auto it = text.cbegin();
    while (std::regex_search(it, text.cend(), m, re)) {
      acc.append(it, m[0].first);
      const auto idx = inlines.size();
      inlines.push_back(
          "<code style=\"font-family:Consolas,monospace;background-color:rgba(127,127,127,0.25);\">" +
          html_escape(m[1].str()) + "</code>");
      acc += std::string("\x01") + "I" + std::to_string(idx) + "\x01";
      it = m[0].second;
    }
    acc.append(it, text.cend());
    text = std::move(acc);
  }

  text = html_escape(text);

  {
    static const std::regex re(R"(\|\|([\s\S]+?)\|\|)");
    std::smatch m;
    std::string acc;
    auto it = text.cbegin();
    int spoiler_i = 0;
    while (std::regex_search(it, text.cend(), m, re)) {
      acc.append(it, m[0].first);
      const std::string body = m[1].str();
      if (revealed_spoilers.count(spoiler_i)) {
        acc += "<span style=\"background-color:rgba(127,127,127,0.35);\">" + body + "</span>";
      } else {
        acc += "<a href=\"nyx-spoiler:" + std::to_string(spoiler_i) +
               "\" style=\"color:transparent;background-color:#888888;text-decoration:none;\">" +
               body + "</a>";
      }
      ++spoiler_i;
      it = m[0].second;
    }
    acc.append(it, text.cend());
    text = std::move(acc);
  }

  // Mentions and http links (after escape: [text](nyx-user:hex) stays)
  {
    static const std::regex re(
        R"(\[([^\]]+)\]\((nyx-user:[a-fA-F0-9]{64}|https?://[^)\s]+)\))");
    text = std::regex_replace(text, re, "<a href=\"$2\">$1</a>");
  }
  {
    static const std::regex re(R"(\*\*([^*\n]+)\*\*)");
    text = std::regex_replace(text, re, "<b>$1</b>");
  }
  {
    static const std::regex re(R"(__([^_\n]+)__)");
    text = std::regex_replace(text, re, "<u>$1</u>");
  }
  {
    static const std::regex re(R"(~~([^~\n]+)~~)");
    text = std::regex_replace(text, re, "<s>$1</s>");
  }
  {
    static const std::regex re(R"((^|[^\w*])\*([^*\n]+)\*(?!\*))");
    text = std::regex_replace(text, re, "$1<i>$2</i>");
  }
  {
    static const std::regex re(R"((^|[^\w_])_([^_\n]+)_(?!_))");
    text = std::regex_replace(text, re, "$1<i>$2</i>");
  }

  std::string html;
  bool in_quote = false;
  bool in_ul = false;
  bool in_ol = false;
  auto close_lists = [&]() {
    if (in_ul) {
      html += "</ul>";
      in_ul = false;
    }
    if (in_ol) {
      html += "</ol>";
      in_ol = false;
    }
  };

  std::size_t start = 0;
  int line_i = 0;
  while (start <= text.size()) {
    const auto nl = text.find('\n', start);
    const std::string line =
        text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);

    const bool quote_line = line.rfind("&gt;", 0) == 0;
    std::string quote_body;
    if (quote_line) {
      if (line.size() > 4 && line[4] == ' ')
        quote_body = line.substr(5);
      else
        quote_body = line.substr(4);
    }

    // HR
    if (trim_copy(line) == "---" || trim_copy(line) == "***") {
      if (in_quote) {
        html += "</blockquote>";
        in_quote = false;
      }
      close_lists();
      html += "<hr/>";
    } else if (quote_line) {
      close_lists();
      if (!in_quote) {
        html += "<blockquote style=\"margin:4px 0;padding-left:8px;border-left:3px solid #5288c1;\">";
        in_quote = true;
      } else {
        html += "<br/>";
      }
      html += quote_body;
    } else {
      if (in_quote) {
        html += "</blockquote>";
        in_quote = false;
      }

      // Headings ### ## #
      std::smatch hm;
      static const std::regex hre(R"(^(#{1,3})\s+(.+)$)");
      if (std::regex_match(line, hm, hre)) {
        close_lists();
        const int level = static_cast<int>(hm[1].str().size());
        const int px = level == 1 ? 22 : (level == 2 ? 18 : 16);
        html += "<div style=\"font-weight:700;font-size:" + std::to_string(px) +
                "px;margin:4px 0;\">" + hm[2].str() + "</div>";
      } else if (std::regex_match(line, hm, std::regex(R"(^[-*]\s+(.+)$)"))) {
        if (in_ol) {
          html += "</ol>";
          in_ol = false;
        }
        if (!in_ul) {
          html += "<ul style=\"margin:4px 0;padding-left:18px;\">";
          in_ul = true;
        }
        html += "<li>" + hm[1].str() + "</li>";
      } else if (std::regex_match(line, hm, std::regex(R"(^(\d+)\.\s+(.+)$)"))) {
        if (in_ul) {
          html += "</ul>";
          in_ul = false;
        }
        if (!in_ol) {
          html += "<ol style=\"margin:4px 0;padding-left:18px;\">";
          in_ol = true;
        }
        html += "<li>" + hm[2].str() + "</li>";
      } else {
        close_lists();
        if (line_i > 0 && !html.empty() && html.back() != '>') html += "<br/>";
        else if (line_i > 0 && !html.empty()) html += "<br/>";
        html += line;
      }
    }
    ++line_i;
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  if (in_quote) html += "</blockquote>";
  close_lists();

  for (std::size_t i = 0; i < inlines.size(); ++i)
    replace_all(html, std::string("\x01") + "I" + std::to_string(i) + "\x01", inlines[i]);
  for (std::size_t j = 0; j < maths.size(); ++j)
    replace_all(html, std::string("\x01") + "M" + std::to_string(j) + "\x01", maths[j]);
  for (std::size_t k = 0; k < fences.size(); ++k)
    replace_all(html, std::string("\x01") + "F" + std::to_string(k) + "\x01", fences[k]);

  return html;
}

}  // namespace nyx
