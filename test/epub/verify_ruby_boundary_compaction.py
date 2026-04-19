from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]
PARSEDTEXT_CPP = ROOT / "lib/Epub/Epub/ParsedText.cpp"


def require(pattern: str, text: str, description: str) -> None:
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(description)


def main() -> int:
    text = PARSEDTEXT_CPP.read_text(encoding="utf-8")

    try:
        require(r"kRubyContinuationTightenPx\s*=\s*10\s*;",
                text,
                "ruby continuation tighten constant missing")
        require(r"std::vector<bool>\s+lineWordContinuesVec\s*;",
                text,
                "character-wrap path does not track continuation flags")
        require(r"lineWordContinuesVec\.push_back\(",
                text,
                "continuation flags are not preserved while building character-wrap lines")
        require(r"wordContinues\.erase\(wordContinues\.begin\(\)\);",
                text,
                "character-wrap path does not consume continuation flags with words")
        require(r"if\s*\(\s*lineWordContinuesVec\[i\s*\+\s*1\]\s*\)\s*\{\s*gap\s*=\s*std::max\(\s*0\s*,\s*gap\s*-\s*getRubyContinuationTighten",
                text,
                "ruby continuation gap is not compacted in line positioning")
        require(r"if\s*\(\s*charsFit\s*>\s*0\s*\)\s*\{[\s\S]*?const\s+bool\s+attachToPrevious\s*=\s*wordContinues\.front\(\)\s*;[\s\S]*?lineWordContinuesVec\.push_back\(attachToPrevious\)",
                text,
                "partial split path does not preserve continuation flags")
        require(r"// Add partial[\s\S]*?const\s+bool\s+attachToPrevious\s*=\s*wordContinues\.front\(\)\s*;[\s\S]*?lineWordContinuesVec\.push_back\(attachToPrevious\)",
                text,
                "phase 2 partial fill path does not preserve continuation flags")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby boundary compaction verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
