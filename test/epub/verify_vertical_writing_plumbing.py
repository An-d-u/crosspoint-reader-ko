#!/usr/bin/env python3
"""
EPUB 원본의 세로쓰기 선언을 존중하는 설정/파서/렌더링 배선이 있는지 확인한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8-sig")


def require(pattern: str, path: str, label: str, failures: list[str]) -> None:
    if not re.search(pattern, read(path), re.DOTALL):
        failures.append(f"{label}: {path}")


def main() -> int:
    failures: list[str] = []

    require(r"respectEpubVerticalWriting\s*=\s*0", "src/CrossPointSettings.h", "settings field missing", failures)
    require(
        r"STR_RESPECT_EPUB_VERTICAL_WRITING[\s\S]*respectEpubVerticalWriting",
        "src/SettingsList.h",
        "reader settings toggle missing",
        failures,
    )
    require(
        r"STR_RESPECT_EPUB_VERTICAL_WRITING",
        "lib/I18n/I18nKeys.h",
        "i18n key missing",
        failures,
    )
    require(
        r"enum\s+class\s+CssWritingMode[\s\S]*VerticalRl[\s\S]*writingMode",
        "lib/Epub/Epub/css/CssStyle.h",
        "CSS writing-mode field missing",
        failures,
    )
    require(
        r"(?=[\s\S]*writing-mode)(?=[\s\S]*vertical-rl)",
        "lib/Epub/Epub/css/CssParser.cpp",
        "CSS writing-mode parsing missing",
        failures,
    )
    require(
        r"hasVerticalWritingMode\s*\(",
        "lib/Epub/Epub/css/CssParser.h",
        "CSS vertical-mode aggregate query missing",
        failures,
    )
    require(
        r"respectEpubVerticalWriting[\s\S]*verticalWritingMode",
        "lib/Epub/Epub/Section.cpp",
        "section cache/build vertical wiring missing",
        failures,
    )
    require(
        r"SETTINGS\.respectEpubVerticalWriting",
        "src/activities/reader/EpubReaderActivity.cpp",
        "reader setting not passed to section",
        failures,
    )
    require(
        r"renderVerticalWriting[\s\S]*drawText",
        "lib/Epub/Epub/blocks/TextBlock.cpp",
        "vertical text rendering path missing",
        failures,
    )
    require(
        r"Page::render\([^)]*bool\s+verticalWritingMode",
        "lib/Epub/Epub/Page.cpp",
        "page render vertical flag missing",
        failures,
    )

    if failures:
        print("FAIL: vertical writing support plumbing is incomplete.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: vertical writing support plumbing is present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
