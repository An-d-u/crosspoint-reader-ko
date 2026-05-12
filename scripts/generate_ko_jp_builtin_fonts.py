#!/usr/bin/env python3
"""
KoPubWorld 기본 폰트에 일본어 fallback font stack을 결합한 builtin header를 생성한다.

기본 정책:
- UI: KoPubWorld Dotum, 일본어는 Yu Mincho
- Reader: KoPubWorld Batang, 일본어는 Yu Mincho
- 성능 우선: kana + 핵심 일본어 기호 + 실제 책/일반 일본어 보강 한자만 비압축으로 추가
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from fontTools.ttLib import TTCollection, TTFont

from japanese_kanji_whitelist import (
    CJK_END,
    CJK_START,
    collect_book_cjk_codepoints,
    collect_book_non_cjk_codepoints,
    collect_common_japanese_codepoints,
    merge_codepoints_to_intervals,
)


ROOT = Path(__file__).resolve().parents[1]
FONTCONVERT = ROOT / "lib" / "EpdFont" / "scripts" / "fontconvert.py"
BUILTIN_DIR = ROOT / "lib" / "EpdFont" / "builtinFonts"
USER_FONT_DIR = ROOT / "fonts"
CACHE_FONT_DIR = ROOT / ".cache" / "generated-fonts"
BOOKS_DIR = ROOT / "books"

KOREAN_INTERVALS = [
    "0x1100,0x11FF",
    "0x3130,0x318F",
    "0xAC00,0xD7A3",
]

JAPANESE_INTERVALS = [
    "0x3000,0x303F",
    "0x3040,0x309F",
    "0x30A0,0x30FF",
    "0x31F0,0x31FF",
    "0xFF01,0xFF9F",
]

EXCLUDED_INTERVALS = [
    "0x0100,0x017F",
    "0x01A0,0x01A1",
    "0x01AF,0x01B0",
    "0x01C4,0x021F",
    "0x0370,0x03FF",
    "0x0400,0x04FF",
    "0x0590,0x05FF",
    "0x0600,0x06FF",
    "0x0900,0x097F",
    "0x0E00,0x0E7F",
    "0x1EA0,0x1EF9",
    "0x2800,0x28FF",
]


@dataclass(frozen=True)
class FontJob:
    name: str
    size: int
    primary_path: Path
    fallback_path: Path
    output_path: Path
    ttc_index: int | None = None


def build_jobs() -> list[FontJob]:
    return [
        FontJob(
            name="kopubworld_dotum_jp_10_regular",
            size=10,
            primary_path=USER_FONT_DIR / "KoPubWorld Dotum_Pro Medium.otf",
            fallback_path=Path(r"C:\Windows\Fonts\yumin.ttf"),
            output_path=BUILTIN_DIR / "kopubworld_dotum_jp_10_regular.h",
        ),
        FontJob(
            name="kopubworld_batang_jp_14_regular",
            size=14,
            primary_path=USER_FONT_DIR / "KoPubWorld Batang_Pro Medium.otf",
            fallback_path=Path(r"C:\Windows\Fonts\yumin.ttf"),
            output_path=BUILTIN_DIR / "kopubworld_batang_jp_14_regular.h",
        ),
    ]


def ensure_exists(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"Required font file not found: {path}")


def extract_ttc_face(ttc_path: Path, face_index: int, output_path: Path) -> Path:
    collection = TTCollection(str(ttc_path))
    if face_index >= len(collection.fonts):
        raise IndexError(f"TTC face index out of range for {ttc_path}: {face_index}")
    collection.fonts[face_index].save(str(output_path))
    return output_path


def load_font_cjk_codepoints(font_path: Path) -> set[int]:
    font = TTFont(str(font_path))
    try:
        cmap = font.getBestCmap() or {}
        return {cp for cp in cmap.keys() if CJK_START <= cp <= CJK_END}
    finally:
        font.close()


def load_font_codepoints(font_path: Path) -> set[int]:
    font = TTFont(str(font_path))
    try:
        return set((font.getBestCmap() or {}).keys())
    finally:
        font.close()


def format_intervals(intervals: list[tuple[int, int]]) -> list[str]:
    return [f"0x{start:X},0x{end:X}" for start, end in intervals]


def build_cjk_intervals(job: FontJob, resolved_fallback_path: Path) -> list[str]:
    primary_cjk = load_font_cjk_codepoints(job.primary_path)
    fallback_cjk = {cp for cp in load_font_codepoints(resolved_fallback_path) if CJK_START <= cp <= CJK_END}
    book_cjk = collect_book_cjk_codepoints(BOOKS_DIR)
    common_cjk = collect_common_japanese_codepoints()

    supplemental_cjk = ((book_cjk | common_cjk) - primary_cjk) & fallback_cjk
    merged_cjk = primary_cjk | supplemental_cjk
    return format_intervals(merge_codepoints_to_intervals(merged_cjk))


def build_forced_non_cjk_intervals(job: FontJob, resolved_fallback_path: Path) -> list[str]:
    primary_all = load_font_codepoints(job.primary_path)
    fallback_all = load_font_codepoints(resolved_fallback_path)
    book_non_cjk = collect_book_non_cjk_codepoints(BOOKS_DIR)
    exportable_non_cjk = book_non_cjk & (primary_all | fallback_all)
    return format_intervals(merge_codepoints_to_intervals(exportable_non_cjk))


def build_fallback_only_intervals(resolved_fallback_path: Path) -> list[str]:
    fallback_all = load_font_codepoints(resolved_fallback_path)
    japanese_non_cjk: set[int] = set()
    for interval in JAPANESE_INTERVALS:
        start, end = (int(n, base=0) for n in interval.split(","))
        japanese_non_cjk.update(range(start, end + 1))

    book_cjk = collect_book_cjk_codepoints(BOOKS_DIR)
    common_cjk = collect_common_japanese_codepoints()
    fallback_only = (japanese_non_cjk | book_cjk | common_cjk) & fallback_all
    return format_intervals(merge_codepoints_to_intervals(fallback_only))


def build_fontconvert_command(job: FontJob, resolved_fallback_path: Path, compress: bool) -> list[str]:
    cjk_intervals = build_cjk_intervals(job, resolved_fallback_path)
    forced_non_cjk_intervals = build_forced_non_cjk_intervals(job, resolved_fallback_path)
    fallback_only_intervals = build_fallback_only_intervals(resolved_fallback_path)
    interval_file = CACHE_FONT_DIR / f"{job.name}-additional-intervals.txt"
    forced_interval_file = CACHE_FONT_DIR / f"{job.name}-forced-intervals.txt"
    fallback_only_interval_file = CACHE_FONT_DIR / f"{job.name}-fallback-only-intervals.txt"
    interval_file.parent.mkdir(parents=True, exist_ok=True)
    interval_file.write_text(
        "\n".join(KOREAN_INTERVALS + JAPANESE_INTERVALS + cjk_intervals) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    forced_interval_file.write_text(
        "\n".join(forced_non_cjk_intervals) + ("\n" if forced_non_cjk_intervals else ""),
        encoding="utf-8",
        newline="\n",
    )
    fallback_only_interval_file.write_text(
        "\n".join(fallback_only_intervals) + ("\n" if fallback_only_intervals else ""),
        encoding="utf-8",
        newline="\n",
    )

    command = [
        sys.executable,
        str(FONTCONVERT),
        job.name,
        str(job.size),
        str(job.primary_path),
        str(resolved_fallback_path),
        "--2bit",
        "--additional-intervals-file",
        str(interval_file),
        "--forced-intervals-file",
        str(forced_interval_file),
        "--fallback-only-intervals-file",
        str(fallback_only_interval_file),
    ]
    if compress:
        command.append("--compress")
    for interval in EXCLUDED_INTERVALS:
        command.extend(["--exclude-intervals", interval])
    return command


def run_job(job: FontJob, check_only: bool, compress: bool) -> None:
    ensure_exists(job.primary_path)
    ensure_exists(job.fallback_path)

    resolved_fallback = job.fallback_path
    if job.ttc_index is not None:
        CACHE_FONT_DIR.mkdir(parents=True, exist_ok=True)
        extracted_path = CACHE_FONT_DIR / f"{job.name}-fallback.ttf"
        resolved_fallback = extract_ttc_face(job.fallback_path, job.ttc_index, extracted_path)

    command = build_fontconvert_command(job, resolved_fallback, compress)
    print(f"[FONT] {job.name}")
    print(f"  primary  : {job.primary_path}")
    print(f"  fallback : {resolved_fallback}")
    print(f"  output   : {job.output_path}")

    if check_only:
        print(f"  command  : {' '.join(command)}")
        return

    with job.output_path.open("w", encoding="utf-8", newline="\n") as output_file:
        subprocess.run(command, cwd=ROOT, stdout=output_file, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate builtin KoPubWorld + Japanese fallback font headers")
    parser.add_argument("--check", action="store_true", help="Resolve inputs and print commands without generating")
    parser.add_argument("--compress", action="store_true", help="Generate compressed grouped fonts instead of the default uncompressed fonts")
    parser.add_argument("--uncompressed", action="store_true", help="Deprecated compatibility flag; uncompressed is already the default")
    args = parser.parse_args()
    if args.compress and args.uncompressed:
        parser.error("--compress and --uncompressed cannot be used together")
    compress = args.compress

    for job in build_jobs():
        run_job(job, check_only=args.check, compress=compress)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
