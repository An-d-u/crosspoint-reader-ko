#!/usr/bin/env python3
"""실제 TXT·MD 변환기의 C++20 정적 단언 및 독서 화면 연결을 검증한다."""

from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        candidates = list((Path.home() / ".platformio/packages/toolchain-riscv32-esp/bin").glob("*g++.exe"))
        if candidates:
            compiler = str(candidates[0])
    if not compiler:
        raise RuntimeError("C++20 컴파일러를 찾지 못했습니다")
    subprocess.run(
        [compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "src"), str(ROOT / "test/reader/TextDocumentMarkupTest.cpp")],
        check=True,
    )
    reader = (ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
    for token in (
        "ChapterHtmlSlimParser parser(", "TextDocumentMarkup::Converter converter(sink, markdown)",
        "cachedExtraParagraphSpacing, cachedParagraphIndent, cachedParagraphAlignment, cachedCharacterWrap",
        "viewportWidth, viewportHeight, cachedHyphenation", "page->serialize(pages)",
        "Page::deserialize(pageFile)", "page.render(renderer", "ReaderUtils::renderAntiAliased",
        "SETTINGS.extraParagraphSpacing != cachedExtraParagraphSpacing",
        "SETTINGS.paragraphIndent != cachedParagraphIndent", "SETTINGS.hyphenationEnabled != cachedHyphenation",
        "header[14] != pageFile.size()", "index.size() != sizeof(header)", "!reservePageOffset(pageOffsets)",
        "pages.hasWriteError()", "if (pageFile.isOpen()) pageFile.close();",
        'CssParser inlineStyles("")', "false, nullptr, &inlineStyles)",
    ):
        assert token in reader, f"독서 기능 연결 누락: {token}"
    assert reader.index('Storage.remove((txt->getCachePath() + "/index.bin")') < reader.index("Converter converter")
    assert "pageFile.close();" not in reader.replace("if (pageFile.isOpen()) pageFile.close();", "")
    recent = (ROOT / "src/RecentBookProgress.cpp").read_text(encoding="utf-8")
    assert "TextReaderCache::pageCount" in recent
    print("PASS: TXT 문단, MD 서식, UTF-8·긴 행·코드 경계, 쓰기 실패, 구형/신형 진행률 색인, EPUB 연결")


if __name__ == "__main__":
    main()
