#!/usr/bin/env bash
# Format project C/C++/ObjC sources (excludes build trees and FetchContent deps).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi
mapfile -t files < <(find src include apps tests android \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' -o -name '*.cc' -o -name '*.mm' \) \
  -type f ! -path '*/build/*' 2>/dev/null | sort)
if [[ ${#files[@]} -eq 0 ]]; then
  echo "no sources" >&2
  exit 1
fi
clang-format -i "${files[@]}"
echo "formatted ${#files[@]} files"
