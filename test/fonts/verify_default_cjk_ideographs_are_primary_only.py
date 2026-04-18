#!/usr/bin/env python3
"""
기본 UI/리더 폰트의 CJK Unified Ideographs가 허용된 whitelist 안에만 들어가는지 검사한다.
허용 범위는 primary 원본 한자 + books 스캔 결과 + 공통 일본어 보강 한자다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from fontTools.ttLib import TTCollection, TTFont

sys.path.insert(0, str((Path(__file__).resolve().parents[2] / "scripts")))

from japanese_kanji_whitelist import collect_book_cjk_codepoints, collect_common_japanese_codepoints


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"
PRIMARY_FONT_PATHS = {
    "ui": ROOT / "fonts" / "KoPubWorld Dotum_Pro Medium.otf",
    "reader": ROOT / "fonts" / "KoPubWorld Batang_Pro Medium.otf",
}
FALLBACK_FONT_PATHS = {
    "ui": ROOT / ".cache" / "generated-fonts" / "kopubworld_dotum_jp_10_regular-fallback.ttf",
    "reader": Path(r"C:\Windows\Fonts\yumin.ttf"),
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
        return {cp for cp in (font.getBestCmap() or {}).keys() if CJK_START <= cp <= CJK_END}
    finally:
        font.close()


def load_fallback_codepoints(font_path: Path, role: str) -> set[int]:
    if role == "ui" and font_path.suffix.lower() == ".ttf":
        font = TTFont(str(font_path))
    elif role == "ui":
        font = TTCollection(str(font_path)).fonts[2]
    else:
        font = TTFont(str(font_path))
    try:
        return {cp for cp in (font.getBestCmap() or {}).keys() if CJK_START <= cp <= CJK_END}
    finally:
        if hasattr(font, "close"):
            font.close()


def main() -> int:
    failures: list[str] = []
    headers = load_default_headers()
    book_codepoints = collect_book_cjk_codepoints(ROOT / "books")
    common_codepoints = collect_common_japanese_codepoints()

    for role, header_path in headers.items():
        header_codepoints = expand_overlap(parse_intervals(header_path), CJK_START, CJK_END)
        primary_codepoints = load_primary_codepoints(PRIMARY_FONT_PATHS[role])
        fallback_codepoints = load_fallback_codepoints(FALLBACK_FONT_PATHS[role], role)
        allowed_codepoints = primary_codepoints | (((book_codepoints | common_codepoints) - primary_codepoints) & fallback_codepoints)

        unexpected_codepoints = sorted(header_codepoints - allowed_codepoints)
        missing_allowed_codepoints = sorted(allowed_codepoints - header_codepoints)

        if unexpected_codepoints:
            sample = ", ".join(f"U+{cp:04X}" for cp in unexpected_codepoints[:5])
            failures.append(
                f"{role} default font {header_path.name} includes {len(unexpected_codepoints)} unexpected "
                f"CJK ideographs outside the whitelist (examples: {sample})"
            )
        if missing_allowed_codepoints:
            sample = ", ".join(f"U+{cp:04X}" for cp in missing_allowed_codepoints[:5])
            failures.append(
                f"{role} default font {header_path.name} is missing {len(missing_allowed_codepoints)} "
                f"whitelisted CJK ideographs (examples: {sample})"
            )

    if failures:
        print("FAIL: Default builtin fonts do not match the allowed CJK whitelist.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Default builtin fonts match the allowed CJK whitelist.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
