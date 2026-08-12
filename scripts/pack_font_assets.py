#!/usr/bin/env python3
"""내장 글꼴 헤더의 비트맵을 전용 assets 파티션 이미지로 묶는다."""

from __future__ import annotations

import argparse
import re
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = int.from_bytes(b"CPAS", "little")
FORMAT_VERSION = 1
HEADER_SIZE = 64
HEADER_STRUCT = struct.Struct("<IHHII" + "III" * 2 + "6I")


@dataclass(frozen=True)
class FontSource:
    name: str
    header: Path


def fnv1a(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def extract_bitmap(source: FontSource) -> bytes:
    text = source.header.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"static\s+const\s+uint8_t\s+{re.escape(source.name)}Bitmaps\[(\d+)\]\s*=\s*\{{(.*?)\n\}};",
        re.DOTALL,
    )
    match = pattern.search(text)
    if not match:
        raise RuntimeError(f"비트맵 배열을 찾을 수 없습니다: {source.header}")

    expected_size = int(match.group(1))
    bitmap = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(2)))
    if len(bitmap) != expected_size:
        raise RuntimeError(
            f"{source.name} 비트맵 크기가 일치하지 않습니다: 선언 {expected_size:,}, 실제 {len(bitmap):,}"
        )
    return bitmap


def build_asset_image(project_dir: Path, output: Path) -> tuple[int, int, int]:
    fonts_dir = project_dir / "lib" / "EpdFont" / "builtinFonts"
    sources = (
        FontSource("kopubworld_dotum_jp_10_regular", fonts_dir / "kopubworld_dotum_jp_10_regular.h"),
        FontSource("kopubworld_batang_jp_14_regular", fonts_dir / "kopubworld_batang_jp_14_regular.h"),
    )
    ui_bitmap, reader_bitmap = (extract_bitmap(source) for source in sources)

    ui_offset = HEADER_SIZE
    reader_offset = ui_offset + len(ui_bitmap)
    total_size = reader_offset + len(reader_bitmap)
    payload = ui_bitmap + reader_bitmap
    header = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER_SIZE,
        total_size,
        fnv1a(payload),
        ui_offset,
        len(ui_bitmap),
        fnv1a(ui_bitmap),
        reader_offset,
        len(reader_bitmap),
        fnv1a(reader_bitmap),
        0,
        0,
        0,
        0,
        0,
        0,
    )
    image = header + payload

    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != image:
        output.write_bytes(image)

    return len(image), len(ui_bitmap), len(reader_bitmap)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    total, ui_size, reader_size = build_asset_image(args.project_dir.resolve(), args.output.resolve())
    print(
        f"글꼴 assets 이미지 생성: {args.output} "
        f"(전체 {total:,}바이트, UI {ui_size:,}바이트, 리더 {reader_size:,}바이트)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
