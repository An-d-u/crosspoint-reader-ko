#!/usr/bin/env python3
"""
최근 추가된 EPUB 기준으로 현재 폰트 소스가 지원 가능한 문자가
기본 리더 폰트에 모두 포함되는지 검사한다.

- 최근 수정 시각 기준 상위 90개 EPUB를 스캔한다.
- U+FFFD는 원본 텍스트 손상 가능성이 높아 제외한다.
- 현재 primary/fallback 폰트가 모두 지원하지 않는 문자는 참고용으로만 집계한다.
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
RECENT_BOOK_LIMIT = 90
CJK_START = 0x4E00
CJK_END = 0x9FFF
EPUB_XML_EXTENSIONS = {".xhtml", ".html", ".htm", ".xml", ".ncx", ".opf"}


def parse_intervals(header_path: Path) -> set[int]:
    content = header_path.read_text(encoding="utf-8")
    codepoints: set[int] = set()
    for start, end in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*0x[0-9A-Fa-f]+\s*\}", content):
        codepoints.update(range(int(start, 16), int(end, 16) + 1))
    return codepoints


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


def should_track_non_cjk(cp: int) -> bool:
    if cp < 0x80:
        return False
    if CJK_START <= cp <= CJK_END:
        return False
    category = unicodedata.category(chr(cp))
    if category.startswith("C"):
        return False
    return True


def iter_recent_epubs() -> list[Path]:
    return sorted(BOOKS_DIR.rglob("*.epub"), key=lambda path: path.stat().st_mtime, reverse=True)[:RECENT_BOOK_LIMIT]


def collect_book_codepoints(epub_path: Path) -> set[int]:
    codepoints: set[int] = set()
    with zipfile.ZipFile(epub_path) as epub:
        for member in epub.namelist():
            if Path(member).suffix.lower() not in EPUB_XML_EXTENSIONS:
                continue
            text = extract_xml_text(epub.read(member))
            codepoints.update(ord(ch) for ch in text)
    return codepoints


def main() -> int:
    if not BOOKS_DIR.exists():
        print("SKIP: books directory not found.")
        return 0

    recent_epubs = iter_recent_epubs()
    if not recent_epubs:
        print("SKIP: no EPUB books found.")
        return 0

    reader_codepoints = parse_intervals(READER_HEADER)
    primary_codepoints = load_font_codepoints(PRIMARY_FONT)
    fallback_codepoints = load_font_codepoints(FALLBACK_FONT)

    supported_missing: dict[int, str] = {}
    unsupported_missing: dict[int, str] = {}

    for epub_path in recent_epubs:
        rel_path = str(epub_path.relative_to(ROOT))
        for cp in collect_book_codepoints(epub_path):
            if cp == 0xFFFD:
                continue
            is_target = (CJK_START <= cp <= CJK_END) or should_track_non_cjk(cp)
            if not is_target or cp in reader_codepoints:
                continue
            if cp in primary_codepoints or cp in fallback_codepoints:
                supported_missing.setdefault(cp, rel_path)
            else:
                unsupported_missing.setdefault(cp, rel_path)

    if supported_missing:
        print("FAIL: recent EPUB books use supported codepoints that are still missing from the reader builtin font.")
        print(f" - recent books scanned: {len(recent_epubs)}")
        print(f" - supported missing codepoints: {len(supported_missing)}")
        for cp, source in sorted(supported_missing.items())[:40]:
            print(f" - U+{cp:04X} source={source}")
        if unsupported_missing:
            print(f" - unsupported by current font sources (reference only): {len(unsupported_missing)}")
        return 1

    print("PASS: recent EPUB books do not use any source-supported codepoints missing from the reader builtin font.")
    print(f" - recent books scanned: {len(recent_epubs)}")
    print(f" - unsupported by current font sources: {len(unsupported_missing)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
