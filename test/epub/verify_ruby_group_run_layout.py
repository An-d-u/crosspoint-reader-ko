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
            "ruby run is not flushed through addWord",
        )
        require(
            r"suppressWhitespaceAfterRuby\s*=\s*true\s*;",
            parser,
            "parser does not suppress formatting whitespace after ruby",
        )
        require(
            r"if\s*\(\s*self->suppressWhitespaceAfterRuby\s*\)\s*\{\s*self->nextWordContinues\s*=\s*true\s*;\s*continue\s*;",
            parser,
            "formatting whitespace after ruby can still break continuation",
        )
        require(
            r"rubyAnnotations\.push_back",
            parsed,
            "ruby run is not stored as a separate annotation",
        )
        require(
            r"auto\s+rubyBaseChars\s*=\s*splitUtf8Chars\(word\)",
            parsed,
            "ruby base text is not split into normal text tokens",
        )
        require(
            r"return\s+baseWidth\s*;",
            parsed,
            "layout width no longer follows base text width",
        )
        forbid(
            r"getRubyPairExtraGap\s*\(",
            parsed,
            "ruby spacing logic still lives in ParsedText instead of the overlay layer",
        )
        require(
            r"auto\s+rubyRuns\s*=\s*buildRubyOverlayRuns\(",
            textblock,
            "ruby overlay runs are not built in a second pass",
        )
        require(
            r"resolveRubyRunOverlaps\s*\(\s*rubyRuns\s*\)",
            textblock,
            "ruby overlay overlaps are not resolved in TextBlock",
        )
        require(
            r"for\s*\(\s*const\s+auto&\s+rubyRun\s*:\s*rubyRuns\s*\)\s*\{\s*renderer\.drawText\(rubyFontId,\s*rubyRun\.x,\s*rubyRun\.y,\s*rubyRun\.text,",
            textblock,
            "ruby overlay runs are not drawn in a dedicated second pass",
        )
        require(
            r"SECTION_FILE_VERSION\s*=\s*29\s*;",
            section,
            "section cache version not bumped for ruby overlay layout",
        )
    except AssertionError as exc:
        print(exc)
        return 1

    print("group ruby run layout verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
