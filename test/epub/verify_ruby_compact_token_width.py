from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PARSEDTEXT_CPP = ROOT / "lib/Epub/Epub/ParsedText.cpp"
TEXTBLOCK_CPP = ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp"
SECTION_CPP = ROOT / "lib/Epub/Epub/Section.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def forbid(pattern: str, text: str, description: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    parsed_text = PARSEDTEXT_CPP.read_text(encoding="utf-8")
    text_block = TEXTBLOCK_CPP.read_text(encoding="utf-8")
    section = SECTION_CPP.read_text(encoding="utf-8")

    try:
        require(r"uint16_t\s+measureRubyWidth\(",
                parsed_text,
                "ruby width helper missing")
        require(r"\(void\)rubyFontId\s*;\s*\(void\)rubyText\s*;\s*return\s+baseWidth\s*;",
                parsed_text,
                "layout width is still inflated by ruby width")
        require(r"if\s*\(\s*leftRubyWidth\s*==\s*0\s*\|\|\s*rightRubyWidth\s*==\s*0\s*\)\s*\{\s*return\s+0\s*;\s*\}",
                parsed_text,
                "ruby pair gap helper should ignore isolated ruby tokens")
        require(r"const\s+int\s+requiredGap\s*=\s*\(\s*rubyOverhangDelta\s*\+\s*1\s*\)\s*/\s*2\s*;",
                parsed_text,
                "ruby pair gap helper does not compute minimal overlap-free spacing")
        require(r"const\s+int\s+centeredBaseX\s*=\s*wordX\s*\+\s*std::max\(\s*0\s*,\s*\(\s*tokenWidth\s*-\s*baseWidth\s*\)\s*/\s*2\s*\)\s*;",
                text_block,
                "base text is not centered from token width")
        require(r"const\s+int\s+centeredRubyX\s*=\s*wordX\s*\+\s*\(\s*tokenWidth\s*-\s*rubyWidth\s*\)\s*/\s*2\s*;",
                text_block,
                "ruby text is not allowed to overhang from base token width")
        require(r"kRubyTextLiftPx\s*=\s*13\s*;", text_block, "ruby lift was not raised by 2px")
        require(r"SECTION_FILE_VERSION\s*=\s*26\s*;", section, "section cache version not bumped for ruby layout change")
    except AssertionError as exc:
        print(exc)
        return 1

    print("compact ruby token width verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
