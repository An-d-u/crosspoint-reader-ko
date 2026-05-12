#!/usr/bin/env python3
"""
일본어 기본 폰트 보강에 사용할 한자 whitelist 유틸리티.

- books/ 아래 실제 EPUB/TXT에서 쓰인 CJK 한자를 수집
- 현대 일본어에서 자주 나오는 보강용 한자를 소규모로 추가
- codepoint 집합을 fontconvert용 interval 목록으로 변환
"""

from __future__ import annotations

import unicodedata
import zipfile
from pathlib import Path
from xml.etree import ElementTree as ET


CJK_START = 0x4E00
CJK_END = 0x9FFF
TEXT_EXTENSIONS = {".txt"}
EPUB_XML_EXTENSIONS = {".xhtml", ".html", ".htm", ".xml", ".ncx", ".opf"}

# KoPub 원본에 없는 경우가 잦은 현대 일본어용 상용 한자/신자체 쪽을 작게 보강한다.
# 실제 books 스캔 결과보다 앞서 넣는 "안전망" 성격의 후보들이다.
COMMON_JAPANESE_KANJI = (
    "青年出会娘森男深歩步"
    "国学図気変実帰広売歩声会当来様対続数真悪医歴鉄駅読転団囲黒黄関観発戦単"
    "価辺証応帯県円写伝薬覚楽静説恋撃礼巻録開閉内両並乗乱仏体児党処剣剤励"
    "労効勉勤勧区厳参双収叙号営嘆圧塀塁壱奥奨宝寝寿専将尽届属岳峡巣庁拡挙"
    "挟挿掲揺摂斉断既旧昼暁晩暑暦条来枢栄桜検楼欧歓歩歯残殻毎沢浄浅浜涙済"
    "渉渋湾湿満滝滞潜瀬炉点焼煮犠状狭独猟献獣瓶画畳発盗砕碑祈福称穏窃竜節"
    "粋経継総緑緒練縁縄縦繁署聴脳臓舗艶芸茎荘著蔵蛍虫蚕蛮衆装褒覇触訳誉諸"
    "謡謹譲豊賃賛践軽辞還郷酔醸釈鉱銭鋭鎮陥険雑雇電響類顔顕験髄黙齢"
)


def is_cjk_ideograph(cp: int) -> bool:
    return CJK_START <= cp <= CJK_END


def extract_xml_text(blob: bytes) -> str:
    try:
        root = ET.fromstring(blob)
        return "".join(root.itertext())
    except ET.ParseError:
        return blob.decode("utf-8", errors="ignore")


def iter_book_texts(books_dir: Path) -> list[tuple[str, str]]:
    texts: list[tuple[str, str]] = []
    if not books_dir.exists():
        return texts

    for path in sorted(books_dir.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() in TEXT_EXTENSIONS:
            texts.append((str(path), path.read_text(encoding="utf-8", errors="ignore")))
            continue
        if path.suffix.lower() != ".epub":
            continue
        with zipfile.ZipFile(path) as epub:
            for member in epub.namelist():
                member_path = Path(member)
                if member_path.suffix.lower() not in EPUB_XML_EXTENSIONS:
                    continue
                texts.append((f"{path}::{member}", extract_xml_text(epub.read(member))))
    return texts


def collect_book_cjk_codepoints(books_dir: Path) -> set[int]:
    codepoints: set[int] = set()
    for _, text in iter_book_texts(books_dir):
        for ch in text:
            cp = ord(ch)
            if is_cjk_ideograph(cp):
                codepoints.add(cp)
    return codepoints


def collect_common_japanese_codepoints() -> set[int]:
    return {ord(ch) for ch in COMMON_JAPANESE_KANJI if is_cjk_ideograph(ord(ch))}


def is_trackable_non_cjk(cp: int) -> bool:
    if cp < 0x80:
        return False
    if is_cjk_ideograph(cp):
        return False
    category = unicodedata.category(chr(cp))
    if category.startswith("C"):
        return False
    return True


def collect_book_non_cjk_codepoints(books_dir: Path) -> set[int]:
    codepoints: set[int] = set()
    for _, text in iter_book_texts(books_dir):
        for ch in text:
            cp = ord(ch)
            if is_trackable_non_cjk(cp):
                codepoints.add(cp)
    return codepoints


def merge_codepoints_to_intervals(codepoints: set[int]) -> list[tuple[int, int]]:
    if not codepoints:
        return []

    sorted_points = sorted(codepoints)
    intervals: list[tuple[int, int]] = []
    start = sorted_points[0]
    end = start

    for cp in sorted_points[1:]:
        if cp == end + 1:
            end = cp
            continue
        intervals.append((start, end))
        start = cp
        end = cp

    intervals.append((start, end))
    return intervals
