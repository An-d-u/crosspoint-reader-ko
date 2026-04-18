#!/usr/bin/env python3
"""
기본 UI/리더 폰트의 CJK Unified Ideographs는 primary 폰트가 원래 가진 글리프만 포함하는지 검사한다.
fallback 전용 한자가 섞이면 성능 우선 정책과 어긋난다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"
PRIMARY_FONT_PATHS = {
    "ui": ROOT / "fonts" / "KoPubWorld Dotum_Pro Medium.otf",
    "reader": ROOT / "fonts" / "KoPubWorld Batang_Pro Medium.otf",
}
CJK_START = 0x4E00
CJK_END = 0x9FFF


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


def expand_overlap(intervals: list[tuple[int, int]], start: int, end: int) -> set[int]:
    codepoints: set[int] = set()
    for interval_start, interval_end in intervals:
        overlap_start = max(start, interval_start)
        overlap_end = min(end, interval_end)
        if overlap_start <= overlap_end:
            codepoints.update(range(overlap_start, overlap_end + 1))
    return codepoints


def load_primary_codepoints(font_path: Path) -> set[int]:
    font = TTFont(str(font_path))
    try:
        return set((font.getBestCmap() or {}).keys())
    finally:
        font.close()


def main() -> int:
    failures: list[str] = []
    headers = load_default_headers()

    for role, header_path in headers.items():
        header_codepoints = expand_overlap(parse_intervals(header_path), CJK_START, CJK_END)
        primary_codepoints = load_primary_codepoints(PRIMARY_FONT_PATHS[role])
        fallback_only_codepoints = sorted(header_codepoints - primary_codepoints)
        if fallback_only_codepoints:
            sample = ", ".join(f"U+{cp:04X}" for cp in fallback_only_codepoints[:5])
            failures.append(
                f"{role} default font {header_path.name} includes {len(fallback_only_codepoints)} fallback-only "
                f"CJK ideographs (examples: {sample})"
            )

    if failures:
        print("FAIL: Default builtin fonts include fallback-only CJK ideographs.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Default builtin fonts only include primary-font CJK ideographs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
