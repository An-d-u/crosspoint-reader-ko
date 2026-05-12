#!/usr/bin/env python3
"""
일본어 kana/일본어 쪽 한자가 KoPub primary에 있어도 Yu Mincho에서 생성되는지 검사한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FONTCONVERT = ROOT / "lib" / "EpdFont" / "scripts" / "fontconvert.py"
GENERATOR = ROOT / "scripts" / "generate_ko_jp_builtin_fonts.py"
BATANG_HEADER = ROOT / "lib" / "EpdFont" / "builtinFonts" / "kopubworld_batang_jp_14_regular.h"
DOTUM_HEADER = ROOT / "lib" / "EpdFont" / "builtinFonts" / "kopubworld_dotum_jp_10_regular.h"

REPRESENTATIVE_CJK = {
    0x6B69: "Japanese 歩",
    0x6B65: "variant 步",
    0x9752: "青",
    0x5E74: "年",
    0x4F1A: "会",
    0x51FA: "出",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_glyphs(header_path: Path) -> dict[int, tuple[int, int, int, int, int, int, int]]:
    text = header_path.read_text(encoding="utf-8-sig")
    pattern = re.compile(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
        r"\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*//\s*U\+([0-9A-Fa-f]+)"
    )
    return {int(match.group(8), 16): tuple(map(int, match.groups()[:7])) for match in pattern.finditer(text)}


def verify_static_plumbing() -> list[str]:
    failures: list[str] = []
    fontconvert = FONTCONVERT.read_text(encoding="utf-8-sig")
    generator = GENERATOR.read_text(encoding="utf-8-sig")

    if "--fallback-only-intervals-file" not in fontconvert:
        failures.append("fontconvert.py does not expose fallback-only interval file support")
    if "face_indices_for_codepoint" not in fontconvert:
        failures.append("fontconvert.py does not route fallback-only glyphs through a shared face selection helper")
    if "fallback_only_ints" not in fontconvert:
        failures.append("fontconvert.py does not parse fallback-only intervals")
    if 'fallback_path=Path(r"C:\\Windows\\Fonts\\yumin.ttf")' not in generator:
        failures.append("generate_ko_jp_builtin_fonts.py should use Yu Mincho as the Japanese fallback")
    if "meiryo.ttc" in generator:
        failures.append("UI Japanese fallback should no longer use Meiryo")
    if "--fallback-only-intervals-file" not in generator:
        failures.append("generated fontconvert command does not pass fallback-only intervals")
    return failures


def verify_header_commands() -> list[str]:
    failures: list[str] = []
    for header in (BATANG_HEADER, DOTUM_HEADER):
        text = header.read_text(encoding="utf-8-sig")
        if r"C:\Windows\Fonts\yumin.ttf" not in text:
            failures.append(f"{header.name} was not generated with Yu Mincho fallback")
        if "--fallback-only-intervals-file" not in text:
            failures.append(f"{header.name} was not generated with fallback-only intervals")
        if "static const EpdFontGroup" in text:
            failures.append(f"{header.name} should stay uncompressed to avoid page-turn decompression latency")
        if re.search(r"^\s*\w+Groups,\s*$", text, re.MULTILINE):
            failures.append(f"{header.name} EpdFontData still references compressed groups")
    return failures


def verify_representative_metrics() -> list[str]:
    failures: list[str] = []
    expected_advances = {
        BATANG_HEADER: 467,
        DOTUM_HEADER: 333,
    }
    for header, expected_advance in expected_advances.items():
        glyphs = parse_glyphs(header)
        for cp, label in REPRESENTATIVE_CJK.items():
            glyph = glyphs.get(cp)
            if glyph is None:
                failures.append(f"{header.name} is missing {label} (U+{cp:04X})")
                continue
            if glyph[2] != expected_advance:
                failures.append(
                    f"{header.name} {label} (U+{cp:04X}) advanceX={glyph[2]}, expected Yu Mincho {expected_advance}"
                )
    return failures


def main() -> int:
    failures = verify_static_plumbing() + verify_header_commands() + verify_representative_metrics()

    if failures:
        print("FAIL: Japanese glyphs are not consistently generated from Yu Mincho.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Japanese kana/CJK representative glyphs are generated from Yu Mincho.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
