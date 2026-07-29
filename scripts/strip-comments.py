#!/usr/bin/env python3
"""Strip narrative comments from C++/Java/QML sources. Keeps NOLINT/pragma/clang-format
and a short allowlist of wire/crypto compatibility warnings."""
from __future__ import annotations

import re
import sys
from pathlib import Path

KEEP_LINE = re.compile(
    r"(?i)(NOLINT|clang-format\s+(on|off)|#\s*pragma|"
    r"wire\s+protocol|on-disk|compatible with data already|"
    r"Escape/unescape must stay|NYXI|Noise\b|do not change|"
    r"must stay compatible|bip39)"
)

def strip_c_style(text: str) -> str:
    out = []
    i = 0
    n = len(text)
    state = "code"  # code, line, block, squote, dquote, raw
    raw_delim = ""
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "code":
            # C++ raw string R"delim( ... )delim"
            if c == "R" and nxt == '"':
                j = i + 2
                while j < n and text[j] != "(":
                    j += 1
                if j < n:
                    raw_delim = text[i + 2 : j]
                    out.append(text[i : j + 1])
                    i = j + 1
                    state = "raw"
                    continue
            if c == "/" and nxt == "/":
                # Do not treat \/\/.../ regex closers as line comments (QML/JS).
                if out and out[-1] == "\\":
                    out.append(c)
                    i += 1
                    continue
                # peek rest of line for keepers
                eol = text.find("\n", i)
                if eol < 0:
                    eol = n
                line = text[i:eol]
                if KEEP_LINE.search(line):
                    out.append(line)
                i = eol
                continue
            if c == "/" and nxt == "*":
                end = text.find("*/", i + 2)
                if end < 0:
                    i = n
                    continue
                block = text[i : end + 2]
                if KEEP_LINE.search(block):
                    out.append(block)
                else:
                    # preserve newlines to keep line numbers roughly stable for diffs? drop fully
                    pass
                i = end + 2
                continue
            if c == "'":
                state = "squote"
                out.append(c)
                i += 1
                continue
            if c == '"':
                state = "dquote"
                out.append(c)
                i += 1
                continue
            out.append(c)
            i += 1
            continue

        if state == "squote":
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == "'":
                state = "code"
            i += 1
            continue

        if state == "dquote":
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"':
                state = "code"
            i += 1
            continue

        if state == "raw":
            closer = ")" + raw_delim + '"'
            pos = text.find(closer, i)
            if pos < 0:
                out.append(text[i:])
                break
            out.append(text[i : pos + len(closer)])
            i = pos + len(closer)
            state = "code"
            continue

    result = "".join(out)
    # collapse >2 blank lines
    result = re.sub(r"\n{3,}", "\n\n", result)
    # trim trailing spaces on lines
    result = "\n".join(line.rstrip() for line in result.splitlines()) + "\n"
    return result


def main() -> int:
    roots = [Path("src"), Path("include/nyx"), Path("apps"), Path("android"), Path("tests")]
    exts = {".cpp", ".hpp", ".h", ".c", ".cc", ".mm", ".java", ".qml"}
    count = 0
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if path.suffix not in exts or not path.is_file():
                continue
            if "build" in path.parts:
                continue
            original = path.read_text(encoding="utf-8", errors="surrogateescape")
            stripped = strip_c_style(original)
            if stripped != original:
                path.write_text(stripped, encoding="utf-8", errors="surrogateescape")
                count += 1
                print(path)
    print(f"updated {count} files", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
