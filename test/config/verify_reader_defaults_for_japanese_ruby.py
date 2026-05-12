from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SETTINGS_H = ROOT / "src/CrossPointSettings.h"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE):
        raise AssertionError(description)


def main() -> int:
    settings = SETTINGS_H.read_text(encoding="utf-8-sig")

    try:
        require(r"uint8_t\s+lineSpacing\s*=\s*WIDE\s*;",
                settings,
                "reader line spacing should default to WIDE for Japanese ruby clearance")
        require(r"uint8_t\s+screenMargin\s*=\s*15\s*;",
                settings,
                "reader screen margin should default to 15 to avoid first-line ruby clipping")
    except AssertionError as exc:
        print(exc)
        return 1

    print("PASS: reader defaults leave enough room for Japanese ruby.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
