#!/usr/bin/env python3
"""Count physical source lines of code in repository-owned files."""

from __future__ import annotations

import subprocess
from collections import defaultdict
from pathlib import Path


LANGUAGES = {
    ".c": ("C", "c"),
    ".cc": ("C++", "c"),
    ".cpp": ("C++", "c"),
    ".cxx": ("C++", "c"),
    ".h": ("C/C++ Header", "c"),
    ".hh": ("C/C++ Header", "c"),
    ".hpp": ("C/C++ Header", "c"),
    ".hxx": ("C/C++ Header", "c"),
    ".java": ("Java", "c"),
    ".js": ("JavaScript", "c"),
    ".qml": ("QML", "c"),
    ".py": ("Python", "hash"),
    ".sh": ("Shell", "hash"),
}


def repository_files(root: Path) -> list[Path]:
    command = ["git", "ls-files", "-co", "--exclude-standard", "-z"]
    result = subprocess.run(command, cwd=root, check=True, capture_output=True)
    return [
        root / name.decode("utf-8", errors="surrogateescape")
        for name in result.stdout.split(b"\0")
        if name
    ]


def strip_c_comments(line: str, in_block: bool) -> tuple[str, bool]:
    output: list[str] = []
    quote = ""
    escaped = False
    index = 0

    while index < len(line):
        if in_block:
            end = line.find("*/", index)
            if end < 0:
                return "".join(output), True
            in_block = False
            index = end + 2
            continue

        char = line[index]
        next_char = line[index + 1] if index + 1 < len(line) else ""

        if quote:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            index += 1
            continue

        if char in {'"', "'", "`"}:
            quote = char
            output.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            break
        elif char == "/" and next_char == "*":
            in_block = True
            index += 2
        else:
            output.append(char)
            index += 1

    return "".join(output), in_block


def strip_hash_comment(line: str) -> str:
    if line.startswith("#!"):
        return line

    output: list[str] = []
    quote = ""
    escaped = False

    for char in line:
        if quote:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
        elif char in {'"', "'"}:
            quote = char
            output.append(char)
        elif char == "#":
            break
        else:
            output.append(char)

    return "".join(output)


def count_file(path: Path, style: str) -> int:
    count = 0
    in_block = False

    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        print(f"warning: cannot read {path}: {error}")
        return 0

    for line in lines:
        if style == "c":
            code, in_block = strip_c_comments(line, in_block)
        else:
            code = strip_hash_comment(line)
        if code.strip():
            count += 1

    return count


def classify(path: Path) -> tuple[str, str] | None:
    if path.name == "CMakeLists.txt" or path.suffix.lower() == ".cmake":
        return "CMake", "hash"
    return LANGUAGES.get(path.suffix.lower())


def main() -> int:
    root = Path(__file__).resolve().parent
    totals: dict[str, list[int]] = defaultdict(lambda: [0, 0])

    try:
        files = repository_files(root)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"error: unable to list repository files: {error}")
        return 1

    for path in files:
        language = classify(path)
        if language is None or not path.is_file():
            continue
        name, style = language
        totals[name][0] += 1
        totals[name][1] += count_file(path, style)

    print(f"{'Language':<16} {'Files':>7} {'SLOC':>10}")
    print("-" * 35)
    for name, (file_count, sloc) in sorted(
        totals.items(), key=lambda item: item[1][1], reverse=True
    ):
        print(f"{name:<16} {file_count:>7} {sloc:>10}")
    print("-" * 35)
    print(
        f"{'Total':<16} "
        f"{sum(value[0] for value in totals.values()):>7} "
        f"{sum(value[1] for value in totals.values()):>10}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
