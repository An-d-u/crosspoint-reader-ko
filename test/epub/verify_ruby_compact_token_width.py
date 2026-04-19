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
        require(r"return\s+baseWidth\s*;", parsed_text, "token width no longer returns base width")
        forbid(r"return\s+std::max\s*\(\s*baseWidth\s*,\s*rubyWidth\s*\)\s*;",
               parsed_text,
               "token width still expands to ruby width")
        forbid(r"const\s+uint16_t\s+rubyWidth\s*=\s*renderer\.getTextAdvanceX\(rubyFontId",
               parsed_text,
               "token width still measures ruby width for layout")
        require(r"const\s+int\s+baseCenterX\s*=\s*centeredBaseX\s*\+\s*\(\s*baseWidth\s*/\s*2\s*\)\s*;",
                text_block,
                "ruby render is not centered from base text position")
        require(r"const\s+int\s+centeredRubyX\s*=\s*baseCenterX\s*-\s*\(\s*rubyWidth\s*/\s*2\s*\)\s*;",
                text_block,
                "ruby render still uses token width centering")
        require(r"kRubyTextLiftPx\s*=\s*13\s*;", text_block, "ruby lift was not raised by 2px")
        require(r"SECTION_FILE_VERSION\s*=\s*24\s*;", section, "section cache version not bumped for ruby layout change")
    except AssertionError as exc:
        print(exc)
        return 1

    print("compact ruby token width verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
