#!/usr/bin/env python3
"""
기본 내장 UI/리더 폰트가 사용하지 않기로 한 스크립트 범위를 포함하지 않는지 검사한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"

EXCLUDED_INTERVALS = {
    "Latin Extended-A": (0x0100, 0x017F),
    "Latin Extended-B (Vietnamese subset)": (0x01A0, 0x01A1),
    "Latin Extended-B (Vietnamese subset 2)": (0x01AF, 0x01B0),
    "Latin Extended-B (European subset)": (0x01C4, 0x021F),
    "Greek": (0x0370, 0x03FF),
    "Cyrillic": (0x0400, 0x04FF),
    "Hebrew": (0x0590, 0x05FF),
    "Arabic": (0x0600, 0x06FF),
    "Devanagari": (0x0900, 0x097F),
    "Thai": (0x0E00, 0x0E7F),
    "Vietnamese Extended": (0x1EA0, 0x1EF9),
    "Braille": (0x2800, 0x28FF),
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


def overlaps(a_start: int, a_end: int, b_start: int, b_end: int) -> bool:
    return not (a_end < b_start or b_end < a_start)


def main() -> int:
    failures: list[str] = []
    for role, header_path in load_default_headers().items():
        intervals = parse_intervals(header_path)
        for label, (excluded_start, excluded_end) in EXCLUDED_INTERVALS.items():
            for start, end in intervals:
                if overlaps(start, end, excluded_start, excluded_end):
                    failures.append(
                        f"{role} default font {header_path.name} overlaps excluded range {label} "
                        f"(U+{excluded_start:04X}-U+{excluded_end:04X}) via U+{start:04X}-U+{end:04X}"
                    )
                    break

    if failures:
        print("FAIL: Default builtin fonts still contain excluded script ranges.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Default builtin fonts exclude the configured unused script ranges.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
