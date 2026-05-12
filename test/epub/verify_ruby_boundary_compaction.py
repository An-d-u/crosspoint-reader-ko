from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PARSEDTEXT_CPP = ROOT / "lib/Epub/Epub/ParsedText.cpp"
TEXTBLOCK_CPP = ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def forbid(pattern: str, text: str, description: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    parsed = PARSEDTEXT_CPP.read_text(encoding="utf-8")
    textblock = TEXTBLOCK_CPP.read_text(encoding="utf-8")

    try:
        require(r"kRubyContinuationTightenPx\s*=\s*10\s*;",
                parsed,
                "ruby continuation tighten constant missing")
        require(r"std::vector<bool>\s+lineWordContinuesVec\s*;",
                parsed,
                "character-wrap path does not track continuation flags")
        require(r"lineWordContinuesVec\.push_back\(",
                parsed,
                "continuation flags are not preserved while building character-wrap lines")
        require(r"wordContinues\.erase\(wordContinues\.begin\(\)\);",
                parsed,
                "character-wrap path does not consume continuation flags with words")
        require(r"if\s*\(\s*lineWordContinuesVec\[i\s*\+\s*1\]\s*\)\s*\{\s*gap\s*=\s*std::max\(\s*0\s*,\s*gap\s*-\s*getRubyContinuationTighten",
                parsed,
                "ruby continuation gap is not compacted in line positioning")
        forbid(r"getRubyPairExtraGap\s*\(",
               parsed,
               "ParsedText still applies ruby overlap spacing in the base text layer")
        require(r"placeRubyCluster\s*\(",
                textblock,
                "TextBlock missing ruby cluster placement helper")
        require(r"resolveRubyRunOverlaps\s*\(",
                textblock,
                "TextBlock missing ruby overlap resolution helper")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby boundary compaction verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
