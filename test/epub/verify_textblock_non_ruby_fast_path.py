from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "lib" / "Epub" / "Epub" / "blocks" / "TextBlock.cpp"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")

    fast_path = re.search(
        r"if\s*\(!blockHasRuby\)\s*\{[\s\S]*?renderer\.drawText\(fontId,\s*wordX,\s*y,\s*words\[i\]\.c_str\(\),\s*true,\s*currentStyle\);",
        text,
    )
    if not fast_path:
        print("Missing non-ruby fast render path in TextBlock::render()", file=sys.stderr)
        return 1

    print("OK: non-ruby fast render path exists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
