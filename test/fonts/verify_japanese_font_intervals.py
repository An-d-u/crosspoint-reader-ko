#!/usr/bin/env python3
"""
기본 내장 UI/리더 폰트 header가 일본어 대표 코드포인트를 포함하는지 검사한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"

INCLUDE_PATTERNS = {
    "ui": re.compile(r'#include <builtinFonts/([^>]+)>\s*$', re.MULTILINE),
    "reader": re.compile(r'#include <builtinFonts/([^>]+)>\s*$', re.MULTILINE),
}

REQUIRED_CODEPOINTS = {
    "ui": {
        0x3002: "Japanese full stop",
        0x300C: "Japanese opening quote",
        0x3042: "Hiragana A",
        0x30A2: "Katakana A",
        0x30FC: "Katakana-Hiragana prolonged sound mark",
        0x56FD: "Kanji 国",
        0x5B66: "Kanji 学",
        0x56F3: "Kanji 図",
        0x6C17: "Kanji 気",
        0x5909: "Kanji 変",
    },
    "reader": {
        0x3002: "Japanese full stop",
        0x300C: "Japanese opening quote",
        0x3042: "Hiragana A",
        0x30A2: "Katakana A",
        0x30FC: "Katakana-Hiragana prolonged sound mark",
        0x56FD: "Kanji 国",
        0x5B66: "Kanji 学",
        0x56F3: "Kanji 図",
        0x6C17: "Kanji 気",
        0x5909: "Kanji 変",
    },
}


def load_default_headers() -> dict[str, Path]:
    content = ALL_H.read_text(encoding="utf-8")
    includes = re.findall(r'#include <builtinFonts/([^>]+)>', content)
    if len(includes) < 3:
        raise RuntimeError(f"Expected at least 3 builtin font includes in {ALL_H}")

    ui_fallback = next((name for name in includes if name == "kopubworld_dotum_jp_10_regular.h"), None)
    reader = next((name for name in includes if name == "kopubworld_batang_jp_14_regular.h"), None)
    if ui_fallback is None or reader is None:
        raise RuntimeError(f"Expected UI JP fallback and reader JP headers in {ALL_H}")

    return {
        "ui": ALL_H.parent / ui_fallback,
        "reader": ALL_H.parent / reader,
    }


def parse_intervals(header_path: Path) -> list[tuple[int, int]]:
    content = header_path.read_text(encoding="utf-8")
    return [
        (int(start, 16), int(end, 16))
        for start, end in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*0x[0-9A-Fa-f]+\s*\}", content)
    ]


def has_codepoint(intervals: list[tuple[int, int]], codepoint: int) -> bool:
    return any(start <= codepoint <= end for start, end in intervals)


def main() -> int:
    headers = load_default_headers()
    failures: list[str] = []

    for font_role, header_path in headers.items():
        intervals = parse_intervals(header_path)
        for codepoint, label in REQUIRED_CODEPOINTS[font_role].items():
            if not has_codepoint(intervals, codepoint):
                failures.append(
                    f"{font_role} default font {header_path.name} is missing {label} (U+{codepoint:04X})"
                )

    if failures:
        print("FAIL: Japanese builtin font coverage is incomplete.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Japanese builtin font coverage is present for default UI and reader fonts.")
    for font_role, header_path in headers.items():
        print(f" - {font_role}: {header_path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
