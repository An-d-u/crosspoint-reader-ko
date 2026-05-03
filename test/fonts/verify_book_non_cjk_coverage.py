#!/usr/bin/env python3
"""
books/ 아래 실제 EPUB/TXT를 스캔해 현재 리더 기본 폰트에 없는 비한자 문자가 남아있는지 검사한다.
ASCII, CJK 한자, 제어문자는 제외한다.
"""

from __future__ import annotations

import re
import sys
import unicodedata
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET

from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[2]
BOOKS_DIR = ROOT / "books"
READER_HEADER = ROOT / "lib" / "EpdFont" / "builtinFonts" / "kopubworld_batang_jp_14_regular.h"
PRIMARY_FONT = ROOT / "fonts" / "KoPubWorld Batang_Pro Medium.otf"
FALLBACK_FONT = Path(r"C:\Windows\Fonts\yumin.ttf")
CJK_START = 0x4E00
CJK_END = 0x9FFF
TEXT_EXTENSIONS = {".txt"}
EPUB_XML_EXTENSIONS = {".xhtml", ".html", ".htm", ".xml", ".ncx", ".opf"}


def parse_intervals(header_path: Path) -> set[int]:
    content = header_path.read_text(encoding="utf-8")
    codepoints: set[int] = set()
    for start, end in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*0x[0-9A-Fa-f]+\s*\}", content):
        codepoints.update(range(int(start, 16), int(end, 16) + 1))
    return codepoints


def has_codepoint(codepoints: set[int], cp: int) -> bool:
    return cp in codepoints


def load_font_codepoints(font_path: Path) -> set[int]:
    font = TTFont(str(font_path))
    try:
        return set((font.getBestCmap() or {}).keys())
    finally:
        font.close()


def extract_xml_text(blob: bytes) -> str:
    try:
        root = ET.fromstring(blob)
        return "".join(root.itertext())
    except ET.ParseError:
        return blob.decode("utf-8", errors="ignore")


def iter_book_texts() -> list[tuple[str, str]]:
    texts: list[tuple[str, str]] = []
    for path in sorted(BOOKS_DIR.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() in TEXT_EXTENSIONS:
            texts.append((str(path.relative_to(ROOT)), path.read_text(encoding="utf-8", errors="ignore")))
            continue
        if path.suffix.lower() != ".epub":
            continue
        with zipfile.ZipFile(path) as epub:
            for member in epub.namelist():
                member_path = Path(member)
                if member_path.suffix.lower() not in EPUB_XML_EXTENSIONS:
                    continue
                texts.append((f"{path.relative_to(ROOT)}::{member}", extract_xml_text(epub.read(member))))
    return texts


def should_track(cp: int) -> bool:
    if cp < 0x80:
        return False
    if CJK_START <= cp <= CJK_END:
        return False
    category = unicodedata.category(chr(cp))
    if category.startswith("C"):
        return False
    return True


def main() -> int:
    if not BOOKS_DIR.exists():
        print("SKIP: books directory not found.")
        return 0

    texts = iter_book_texts()
    if not texts:
        print("SKIP: no EPUB/TXT books found.")
        return 0

    intervals = parse_intervals(READER_HEADER)
    source_supported = load_font_codepoints(PRIMARY_FONT) | load_font_codepoints(FALLBACK_FONT)
    supported_missing_sources: dict[int, str] = {}
    unsupported_sources: dict[int, str] = {}

    for source, text in texts:
        for ch in set(text):
            cp = ord(ch)
            if cp == 0xFFFD:
                continue
            if not should_track(cp):
                continue
            if has_codepoint(intervals, cp):
                continue
            if cp in source_supported:
                supported_missing_sources.setdefault(cp, source)
            else:
                unsupported_sources.setdefault(cp, source)

    if supported_missing_sources:
        print("FAIL: current reader builtin font is missing source-supported non-CJK codepoints used by local books.")
        print(f" - unique supported missing codepoints: {len(supported_missing_sources)}")
        for cp in sorted(supported_missing_sources)[:20]:
            print(f" - U+{cp:04X} source={supported_missing_sources[cp]}")
        if unsupported_sources:
            print(f" - unsupported by current font sources (reference only): {len(unsupported_sources)}")
        return 1

    print("PASS: local books do not use any source-supported non-CJK codepoints missing from the current reader builtin font.")
    print(f" - unsupported by current font sources: {len(unsupported_sources)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
