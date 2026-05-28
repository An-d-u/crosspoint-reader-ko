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
    parsed = PARSEDTEXT_CPP.read_text(encoding="utf-8")
    textblock = TEXTBLOCK_CPP.read_text(encoding="utf-8")
    section = SECTION_CPP.read_text(encoding="utf-8")

    try:
        forbid(r"getRubyPairExtraGap\s*\(", parsed, "ParsedText still adjusts base layout spacing for ruby")
        forbid(r"measureRubyWidth\s*\(", parsed, "ParsedText still measures ruby width during base layout")
        require(r"rubyAnnotations", parsed, "ParsedText missing separate ruby annotation storage")
        require(r"rubyAnnotations\.push_back", parsed, "ParsedText does not create ruby annotations")
        require(r"struct\s+RubyOverlayRun", textblock, "TextBlock missing ruby overlay run structure")
        require(r"buildRubyOverlayRuns\s*\(", textblock, "TextBlock missing ruby overlay run builder")
        require(r"resolveRubyRunOverlaps\s*\(", textblock, "TextBlock missing ruby overlap resolver")
        require(r"clampRubyRunsToScreen\s*\(", textblock, "TextBlock missing ruby edge clipping guard")
        require(r"auto\s+rubyRuns\s*=\s*buildRubyOverlayRuns\(", textblock, "TextBlock render does not build ruby overlay runs")
        require(r"resolveRubyRunOverlaps\s*\(\s*rubyRuns\s*\)", textblock, "TextBlock render does not resolve ruby overlay collisions")
        require(r"clampRubyRunsToScreen\s*\(\s*rubyRuns\s*,\s*renderer\.getScreenWidth\(\)\s*\)",
                textblock, "TextBlock render does not keep ruby overlay inside the screen")
        require(r"for\s*\(\s*const\s+auto&\s+rubyRun\s*:\s*rubyRuns\s*\)", textblock, "TextBlock render does not draw ruby overlay runs in a second pass")
        require(r"SECTION_FILE_VERSION\s*=\s*(?:3[0-9]|[4-9][0-9])\s*;", section,
                "section cache version not bumped for ruby overlay layout")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby overlay layer verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
