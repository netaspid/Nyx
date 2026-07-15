#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
words = (root / "src" / "bip39_english.txt").read_text(encoding="utf-8").split()
assert len(words) == 2048, len(words)
out = root / "src" / "bip39_english.inc"
lines = [
    "// Auto-generated BIP39 English wordlist (2048 words).",
    "static const char* const kBip39Words[2048] = {",
]
for i, w in enumerate(words):
    comma = "," if i + 1 < len(words) else ""
    lines.append(f'  "{w}"{comma}')
lines.append("};")
out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
print(f"wrote {out} ({len(words)} words)")
