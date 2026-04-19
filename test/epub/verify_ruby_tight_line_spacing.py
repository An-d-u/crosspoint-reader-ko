from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
TEXTBLOCK_CPP = ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def forbid(pattern: str, text: str, description: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    text = TEXTBLOCK_CPP.read_text(encoding="utf-8")

    try:
        require(r"kRubyLineExtraPx", text, "tight ruby line height constant missing")
        require(r"kRubyBaseYOffsetPx", text, "ruby base offset constant missing")
        require(r"kRubyTextLiftPx", text, "ruby lift constant missing")
        forbid(r"return\s+baseLineHeight\s*\+\s*renderer\.getLineHeight\(rubyFontId\)\s*;",
               text,
               "ruby line height still adds full ruby font height")
        forbid(r"drawText\(rubyFontId,\s*centeredRubyX,\s*y,", text,
               "ruby text still renders directly at line top without lift")
    except AssertionError as exc:
        print(exc)
        return 1

    print("tight ruby line spacing verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
