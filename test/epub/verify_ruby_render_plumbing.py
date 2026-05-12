from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(pattern: str, path: Path, description: str) -> None:
    text = path.read_text(encoding="utf-8")
    if not re.search(pattern, text, re.MULTILINE | re.DOTALL):
        raise AssertionError(f"{description} missing in {path}")


def main() -> int:
    try:
        require(r"rubyAnnotations", ROOT / "lib/Epub/Epub/ParsedText.h", "ParsedText ruby annotation storage")
        require(r"RubyAnnotation", ROOT / "lib/Epub/Epub/blocks/TextBlock.h", "TextBlock ruby annotation type")
        require(r"rubyAnnotations", ROOT / "lib/Epub/Epub/blocks/TextBlock.h", "TextBlock ruby annotation storage")
        require(r"rubyFontId", ROOT / "lib/Epub/Epub/Page.h", "Page render ruby font plumbing")
        require(r"tagLocalEquals\(name,\s*\"ruby\"\)", ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp",
                "ruby tag handling")
        require(r"tagLocalEquals\(name,\s*\"rt\"\)", ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp",
                "rt tag handling")
        require(r"page->render\(renderer,\s*SETTINGS\.getReaderFontId\(\),\s*UI_FONT_ID,",
                ROOT / "src/activities/reader/EpubReaderActivity.cpp", "reader ruby font render call")
        require(r"buildRubyOverlayRuns\s*\(", ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp",
                "ruby overlay builder")
        require(r"resolveRubyRunOverlaps\s*\(", ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp",
                "ruby overlay overlap resolver")
        require(r"SECTION_FILE_VERSION\s*=\s*29", ROOT / "lib/Epub/Epub/Section.cpp",
                "section cache version bump for ruby serialization")
    except AssertionError as exc:
        print(exc)
        return 1

    print("ruby render plumbing verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
