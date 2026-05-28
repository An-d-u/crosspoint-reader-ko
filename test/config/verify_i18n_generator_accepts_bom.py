#!/usr/bin/env python3
"""
i18n 생성기가 UTF-8 BOM이 붙은 번역 YAML도 읽을 수 있는지 확인한다.
"""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from gen_i18n import parse_yaml_file  # noqa: E402


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp_dir:
        path = Path(tmp_dir) / "english.yaml"
        path.write_text('_language_name: "English"\nSTR_HELLO: "Hello"\n', encoding="utf-8-sig")

        try:
            parsed = parse_yaml_file(str(path))
        except ValueError as exc:
            print(f"FAIL: BOM translation YAML was rejected: {exc}")
            return 1

    if parsed.get("_language_name") != "English" or parsed.get("STR_HELLO") != "Hello":
        print("FAIL: BOM translation YAML parsed incorrect values.")
        return 1

    print("PASS: i18n generator accepts UTF-8 BOM translation YAML.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
