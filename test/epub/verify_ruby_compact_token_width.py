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
        require(r"\(void\)rubyFontId\s*;\s*\(void\)rubyText\s*;\s*return\s+baseWidth\s*;",
                parsed_text,
                "layout width is still inflated by ruby width")
        forbid(r"measureRubyWidth\s*\(",
               parsed_text,
               "base layout still measures ruby width")
        forbid(r"getRubyPairExtraGap\s*\(",
               parsed_text,
               "base layout still adjusts word spacing for ruby")
        forbid(r"centeredBaseX",
               text_block,
               "ruby render path still recenters base text instead of drawing normal base tokens")
        forbid(r"getTextAdvanceX\(fontId,\s*words\[i\]\.c_str\(\)",
               text_block,
               "ruby render path still remeasures every base token while drawing")
        require(r"struct\s+RubyOverlayRun",
                text_block,
                "ruby overlay run structure missing")
        require(r"buildRubyOverlayRuns\s*\(",
                text_block,
                "ruby overlay builder missing")
        require(r"const\s+int\s+baseRight\s*=\s*wordXpos\[lastIndex\]\s*\+\s*x\s*\+\s*tokenWidths\[lastIndex\]\s*;",
                text_block,
                "ruby overlay does not use the full annotated base span")
        require(r"resolveRubyRunOverlaps\s*\(",
                text_block,
                "ruby overlay overlap resolver missing")
        require(r"const\s+int\s+preferredX\s*=\s*baseX\s*\+\s*\(\s*baseWidth\s*-\s*rubyWidth\s*\)\s*/\s*2\s*;",
                text_block,
                "ruby preferred overlay position missing")
        require(r"kRubyTextLiftPx\s*=\s*19\s*;", text_block, "ruby lift was not raised for Yu Mincho metrics")
        require(r"SECTION_FILE_VERSION\s*=\s*30\s*;", section, "section cache version not bumped for ruby layout change")
    except AssertionError as exc:
        print(exc)
        return 1

    print("compact ruby token width verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
