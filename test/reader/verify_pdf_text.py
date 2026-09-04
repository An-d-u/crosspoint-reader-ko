#!/usr/bin/env python3
"""PDF 텍스트 해석기의 실제 C++ 함수를 정적 단언으로 검증한다."""

from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        compiler = str(next((Path.home() / ".platformio/packages/toolchain-riscv32-esp/bin").glob("*g++.exe")))
    subprocess.run([compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                    "-fconstexpr-ops-limit=100000000", "-I", str(ROOT / "lib/PdfText"),
                    str(ROOT / "test/reader/PdfTextParserTest.cpp")], check=True)
    reader = (ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
    assert "PdfText::extract(txt->getPath(), pdfText.path, txt->getCachePath())" in reader
    assert "pdfError != PdfText::Error::None" in reader
    assert "pdfErrorMessage(pdfError)" in reader
    assert "pdf ? 2u" in reader
    for path in ("src/activities/home/FileBrowserActivity.cpp", "src/activities/reader/ReaderActivity.cpp",
                 "src/BookDataStore.cpp", "src/RecentBookProgress.cpp", "src/RecentBooksStore.cpp"):
        assert "hasPdfExtension" in (ROOT / path).read_text(encoding="utf-8"), path
    print("PASS: PDF 문자표·UTF-16·본문 연산자·페이지 순서·OCR 이미지 건너뛰기·손상 및 자원 한도·독서 연결")


if __name__ == "__main__":
    main()
