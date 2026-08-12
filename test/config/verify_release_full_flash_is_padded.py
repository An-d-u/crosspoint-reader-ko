#!/usr/bin/env python3
"""
릴리스 풀플래시 이미지가 16MB 길이로 패딩되어 생성되는지 확인한다.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"


def main() -> int:
    workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8-sig")
    if "--pad-to-size 16MB" not in workflow:
        print("FAIL: release workflow does not pad merged full-flash image to 16MB.")
        return 1

    if "CrossPoint-${{ github.ref_name }}.bin" not in workflow:
        print("FAIL: release workflow full-flash output name is missing.")
        return 1

    if "0x810000 .pio/build/gh_release/assets.bin" not in workflow:
        print("FAIL: release workflow does not include the external font assets image.")
        return 1

    print("PASS: release workflow pads full-flash image to 16MB.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
