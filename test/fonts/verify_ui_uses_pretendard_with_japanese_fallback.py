#!/usr/bin/env python3
"""
UI 기본 폰트가 Pretendard로 유지되고, 일본어는 별도 UI fallback 폰트를 통해 처리되는지 검사한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"
MAIN_CPP = ROOT / "src" / "main.cpp"


def main() -> int:
    failures: list[str] = []

    all_h = ALL_H.read_text(encoding="utf-8")
    includes = re.findall(r'#include <builtinFonts/([^>]+)>', all_h)

    if "pretendard_10_regular.h" not in includes:
        failures.append("all.h should include pretendard_10_regular.h for UI primary")
    if "kopubworld_dotum_jp_10_regular.h" not in includes:
        failures.append("all.h should include kopubworld_dotum_jp_10_regular.h for UI Japanese fallback")
    if "kopubworld_batang_jp_14_regular.h" not in includes:
        failures.append("all.h should include kopubworld_batang_jp_14_regular.h for reader")

    main_cpp = MAIN_CPP.read_text(encoding="utf-8")
    if "EpdFont pretendard10RegularFont(&pretendard_10_regular);" not in main_cpp:
        failures.append("main.cpp should keep Pretendard as the UI primary flash font")
    if "EpdFont uiJapaneseFallbackFont(&kopubworld_dotum_jp_10_regular);" not in main_cpp:
        failures.append("main.cpp should define a dedicated Japanese UI fallback flash font")
    if "renderer.insertFont(UI_FONT_ID, &uiFontFamily, &uiJapaneseFallbackFamily);" not in main_cpp:
        failures.append("main.cpp should register UI font with an explicit Japanese fallback family")

    if failures:
        print("FAIL: UI font stack is not configured for Pretendard primary + Japanese fallback.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: UI font stack uses Pretendard primary with an explicit Japanese fallback.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
