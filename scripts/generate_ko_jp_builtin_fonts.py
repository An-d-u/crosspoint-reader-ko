#!/usr/bin/env python3
"""
KoPubWorld 기본 폰트에 일본어 fallback font stack을 결합한 builtin header를 생성한다.

기본 정책:
- UI: KoPubWorld Dotum -> Meiryo UI Regular
- Reader: KoPubWorld Batang -> Yu Mincho
- 성능 우선: kana + 핵심 일본어 기호 중심, CJK 한자는 primary 폰트가 원래 가진 글리프만 유지
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from fontTools.ttLib import TTCollection


ROOT = Path(__file__).resolve().parents[1]
FONTCONVERT = ROOT / "lib" / "EpdFont" / "scripts" / "fontconvert.py"
BUILTIN_DIR = ROOT / "lib" / "EpdFont" / "builtinFonts"
USER_FONT_DIR = ROOT / "fonts"
CACHE_FONT_DIR = ROOT / ".cache" / "generated-fonts"

KOREAN_INTERVALS = [
    "0x1100,0x11FF",
    "0x3130,0x318F",
    "0xAC00,0xD7A3",
]

JAPANESE_INTERVALS = [
    "0x3000,0x3002",
    "0x3005,0x3007",
    "0x300C,0x3011",
    "0x301C,0x301C",
    "0x3040,0x309F",
    "0x30A0,0x30FF",
    "0x31F0,0x31FF",
    "0xFF5E,0xFF5E",
    "0xFF60,0xFF9F",
]

CJK_PRIMARY_ONLY_INTERVALS = [
    "0x4E00,0x9FFF",
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
            fallback_path=Path(r"C:\Windows\Fonts\meiryo.ttc"),
            output_path=BUILTIN_DIR / "kopubworld_dotum_jp_10_regular.h",
            ttc_index=2,
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


def build_fontconvert_command(job: FontJob, resolved_fallback_path: Path) -> list[str]:
    command = [
        sys.executable,
        str(FONTCONVERT),
        job.name,
        str(job.size),
        str(job.primary_path),
        str(resolved_fallback_path),
        "--2bit",
        "--compress",
    ]
    for interval in KOREAN_INTERVALS + JAPANESE_INTERVALS + CJK_PRIMARY_ONLY_INTERVALS:
        command.extend(["--additional-intervals", interval])
    for interval in CJK_PRIMARY_ONLY_INTERVALS:
        command.extend(["--primary-only-intervals", interval])
    for interval in EXCLUDED_INTERVALS:
        command.extend(["--exclude-intervals", interval])
    return command


def run_job(job: FontJob, check_only: bool) -> None:
    ensure_exists(job.primary_path)
    ensure_exists(job.fallback_path)

    resolved_fallback = job.fallback_path
    if job.ttc_index is not None:
        CACHE_FONT_DIR.mkdir(parents=True, exist_ok=True)
        extracted_path = CACHE_FONT_DIR / f"{job.name}-fallback.ttf"
        resolved_fallback = extract_ttc_face(job.fallback_path, job.ttc_index, extracted_path)

    command = build_fontconvert_command(job, resolved_fallback)
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
    args = parser.parse_args()

    for job in build_jobs():
        run_job(job, check_only=args.check)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
