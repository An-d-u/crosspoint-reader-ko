from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
TEXTBLOCK_CPP = ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def reject(pattern: str, text: str, description: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    textblock = TEXTBLOCK_CPP.read_text(encoding="utf-8-sig")

    try:
        reject(r"isJapaneseSmallKana", textblock, "small kana should no longer receive special ruby handling")
        reject(r"compactSmallKana", textblock, "small kana width compaction should be removed")
        reject(r"splitRubyTextClusters", textblock, "ruby text should no longer be split only for small kana")
        reject(r"getRubyClusterAdvance", textblock, "small kana advance override should be removed")
        reject(r"measureRubyTextWidth", textblock, "small kana ruby width helper should be removed")
        require(r"renderer\.getTextAdvanceX\(rubyFontId,\s*ruby\.text\.c_str\(\),\s*EpdFontFamily::REGULAR\)",
                textblock,
                "ruby width should use the normal ruby text advance")
        require(r"drawRubyText\(renderer,\s*rubyFontId,\s*rubyRun\)",
                textblock,
                "ruby drawing helper is not used")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby small kana special handling removal verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
