from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
TEXTBLOCK_CPP = ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    textblock = TEXTBLOCK_CPP.read_text(encoding="utf-8-sig")

    try:
        require(r"bool\s+isJapaneseSmallKana\s*\(", textblock, "small kana detector is missing")
        require(r"0x3085", textblock, "hiragana small yu is not handled")
        require(r"0x3063", textblock, "hiragana small tsu is not handled")
        require(r"0x30E5", textblock, "katakana small yu is not handled")
        require(r"splitRubyTextClusters\s*\(", textblock, "ruby text is not split into drawable clusters")
        require(r"utf8IsCombiningMark\(cp\).*clusters\.back\(\)\.text\.append",
                textblock,
                "combining marks are not preserved with their base ruby glyph")
        require(r"measureRubyTextWidth\s*\(", textblock, "ruby text measurement helper is missing")
        require(r"getRubyClusterAdvance\s*\(", textblock, "ruby cluster advance helper is missing")
        require(r"getTextWidth\(rubyFontId,\s*cluster\.text\.c_str\(\),\s*EpdFontFamily::REGULAR\)",
                textblock,
                "small kana advance is not compacted to visual width")
        require(r"drawRubyText\(renderer,\s*rubyFontId,\s*rubyRun\)",
                textblock,
                "ruby drawing does not use small-kana-aware renderer")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby small kana compaction verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
