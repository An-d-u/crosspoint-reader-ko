#!/usr/bin/env python3
"""
기본 내장 폰트가 압축 그룹을 쓸 경우 그룹 크기가 안전한지 검사한다.
비압축 폰트는 그룹이 없으므로 그대로 허용한다.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ALL_H = ROOT / "lib" / "EpdFont" / "builtinFonts" / "all.h"
SAFE_MAX_UNCOMPRESSED_GROUP_BYTES = 64 * 1024


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


def parse_group_sizes(header_path: Path) -> list[int]:
    text = header_path.read_text(encoding="utf-8")
    array_match = re.search(r"static const EpdFontGroup \w+Groups\[\]\s*=\s*\{(.+?)\};", text, re.DOTALL)
    if not array_match:
        return []
    groups = [(int(a), int(b), int(c), int(d), int(e)) for a, b, c, d, e in re.findall(
        r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
        array_match.group(1),
    )]
    return [entry[2] for entry in groups]


def main() -> int:
    failures: list[str] = []
    for role, header_path in load_default_headers().items():
        group_sizes = parse_group_sizes(header_path)
        if not group_sizes:
            continue
        max_group = max(group_sizes)
        if max_group > SAFE_MAX_UNCOMPRESSED_GROUP_BYTES:
            failures.append(
                f"{role} default font {header_path.name} has max group {max_group} bytes "
                f"(limit {SAFE_MAX_UNCOMPRESSED_GROUP_BYTES})"
            )

    if failures:
        print("FAIL: Default compressed font groups are too large for safe startup/rendering.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Default font compression strategy is within the safe limit.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
