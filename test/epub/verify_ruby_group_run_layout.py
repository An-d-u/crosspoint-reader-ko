from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PARSER_CPP = ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp"
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
    parser = PARSER_CPP.read_text(encoding="utf-8")
    parsed = PARSEDTEXT_CPP.read_text(encoding="utf-8")
    textblock = TEXTBLOCK_CPP.read_text(encoding="utf-8")
    section = SECTION_CPP.read_text(encoding="utf-8")

    try:
        require(
            r"currentTextBlock->addWord\(\s*std::move\(rubyBaseBuffer\)\s*,\s*currentFontStyle\(\)\s*,\s*false\s*,\s*nextWordContinues\s*,\s*std::move\(rubyTextBuffer\)\s*\)",
            parser,
            "ruby run is not flushed as a single base/ruby token",
        )
        forbid(
            r"for\s*\(\s*auto&\s*\[\s*baseText\s*,\s*rubyText\s*\]\s*:\s*rubySegments\s*\)",
            parser,
            "ruby flush still iterates per-segment instead of group-run output",
        )
        require(
            r"uint16_t\s+measureRubyWidth\(",
            parsed,
            "ruby width helper missing",
        )
        require(
            r"return\s+baseWidth\s*;",
            parsed,
            "layout width no longer follows base text width",
        )
        require(
            r"int\s+getRubyPairExtraGap\(",
            parsed,
            "ruby pair gap helper missing",
        )
        require(
            r"const\s+int\s+centeredBaseX\s*=\s*wordX\s*\+\s*std::max\(\s*0\s*,\s*\(\s*tokenWidth\s*-\s*baseWidth\s*\)\s*/\s*2\s*\)\s*;",
            textblock,
            "base text is not centered within the shared ruby token width",
        )
        require(
            r"const\s+int\s+centeredRubyX\s*=\s*wordX\s*\+\s*\(\s*tokenWidth\s*-\s*rubyWidth\s*\)\s*/\s*2\s*;",
            textblock,
            "ruby text is not allowed to overhang from the base token width",
        )
        require(
            r"SECTION_FILE_VERSION\s*=\s*26\s*;",
            section,
            "section cache version not bumped for grouped ruby layout",
        )
    except AssertionError as exc:
        print(exc)
        return 1

    print("group ruby run layout verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
