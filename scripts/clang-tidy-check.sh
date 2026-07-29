#!/usr/bin/env bash
# Advisory clang-tidy pass (warnings are not errors yet).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy not found" >&2
  exit 1
fi
BUILD_DIR="${1:-build}"
COMPILE_DB="$BUILD_DIR/compile_commands.json"
if [[ ! -f "$COMPILE_DB" ]]; then
  echo "missing $COMPILE_DB (configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)" >&2
  exit 1
fi
mapfile -t files < <(find src include/nyx apps/nyx-app/appcore tests \
  \( -name '*.cpp' -o -name '*.hpp' \) -type f | sort)
clang-tidy -p "$BUILD_DIR" "${files[@]}" || true
echo "clang-tidy finished (advisory)"
