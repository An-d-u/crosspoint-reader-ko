#!/usr/bin/env python3
"""
books/ 아래 실제 EPUB/TXT를 스캔해 현재 리더 기본 폰트에 없는 CJK 한자가 남아있는지 검사한다.
로컬 책이 없으면 검사를 건너뛴다.
"""

from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
BOOKS_DIR = ROOT / "books"
READER_HEADER = ROOT / "lib" / "EpdFont" / "builtinFonts" / "kopubworld_batang_jp_14_regular.h"
CJK_START = 0x4E00
CJK_END = 0x9FFF
TEXT_EXTENSIONS = {".txt"}
EPUB_XML_EXTENSIONS = {".xhtml", ".html", ".htm", ".xml", ".ncx", ".opf"}


def parse_intervals(header_path: Path) -> list[tuple[int, int]]:
    content = header_path.read_text(encoding="utf-8")
    return [
        (int(start, 16), int(end, 16))
        for start, end in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*0x[0-9A-Fa-f]+\s*\}", content)
    ]


def has_codepoint(intervals: list[tuple[int, int]], cp: int) -> bool:
    return any(start <= cp <= end for start, end in intervals)


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
                text = extract_xml_text(epub.read(member))
                texts.append((f"{path.relative_to(ROOT)}::{member}", text))
    return texts


def main() -> int:
    if not BOOKS_DIR.exists():
        print("SKIP: books directory not found.")
        return 0

    texts = iter_book_texts()
    if not texts:
        print("SKIP: no EPUB/TXT books found.")
        return 0

    intervals = parse_intervals(READER_HEADER)
    missing_counts: dict[int, int] = {}
    missing_sources: dict[int, str] = {}

    for source, text in texts:
        for ch in text:
            cp = ord(ch)
            if not (CJK_START <= cp <= CJK_END):
                continue
            if has_codepoint(intervals, cp):
                continue
            missing_counts[cp] = missing_counts.get(cp, 0) + 1
            missing_sources.setdefault(cp, source)

    if missing_counts:
        print("FAIL: current reader builtin font is missing CJK ideographs used by local books.")
        print(f" - unique missing ideographs: {len(missing_counts)}")
        for cp, count in sorted(missing_counts.items(), key=lambda item: (-item[1], item[0]))[:20]:
            print(f" - U+{cp:04X} count={count} source={missing_sources[cp]}")
        return 1

    print("PASS: local books do not use CJK ideographs missing from the current reader builtin font.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
