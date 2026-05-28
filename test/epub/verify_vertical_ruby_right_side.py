#!/usr/bin/env python3
"""
세로쓰기 모드에서 후리가나가 본문 오른쪽 전용 영역에 배치되는지 확인한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8-sig")


def require(pattern: str, text: str, label: str, failures: list[str]) -> None:
    if not re.search(pattern, text, re.DOTALL):
        failures.append(label)


def main() -> int:
    textblock_cpp = read("lib/Epub/Epub/blocks/TextBlock.cpp")
    textblock_h = read("lib/Epub/Epub/blocks/TextBlock.h")
    parser_cpp = read("lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp")

    failures: list[str] = []
    require(r"kVerticalRubyGapPx", textblock_cpp, "vertical ruby gap constant missing", failures)
    require(
        r"verticalWritingMode[\s\S]*renderer\.getLineHeight\(rubyFontId\)",
        textblock_cpp,
        "vertical line height does not reserve ruby width",
        failures,
    )
    require(
        r"rubyX\s*=\s*x\s*\+\s*renderer\.getLineHeight\(fontId\)\s*\+\s*kVerticalRubyGapPx",
        textblock_cpp,
        "vertical ruby is not placed to the right of the base text",
        failures,
    )
    require(
        r"getRenderedLineHeight\([^;]*bool\s+verticalWritingMode",
        textblock_h,
        "TextBlock line height API does not accept vertical mode",
        failures,
    )
    require(
        r"getRenderedLineHeight\(renderer,\s*fontId,\s*rubyFontId,\s*lineCompression,\s*verticalWritingMode\)",
        parser_cpp,
        "vertical layout does not pass vertical mode into line height calculation",
        failures,
    )

    if failures:
        print("FAIL: vertical ruby right-side layout is incomplete.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: vertical ruby right-side layout is present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
