#!/usr/bin/env python3
"""
생성된 i18n 코드가 영어/한국어만 포함하는지 검사한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
I18N_KEYS = ROOT / "lib" / "I18n" / "I18nKeys.h"


def main() -> int:
    text = I18N_KEYS.read_text(encoding="utf-8")
    enum_match = re.search(r"enum class Language : uint8_t \{(.*?)_COUNT", text, re.DOTALL)
    if not enum_match:
        print("FAIL: Could not parse Language enum from I18nKeys.h")
        return 1

    langs = re.findall(r"\b([A-Z_]+)\s*=\s*\d+\s*,", enum_match.group(1))
    expected = ["EN", "KOREAN"]
    if langs != expected:
        print("FAIL: Language enum is not English/Korean only.")
        print(" expected:", expected)
        print(" actual  :", langs)
        return 1

    print("PASS: i18n is restricted to English and Korean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
