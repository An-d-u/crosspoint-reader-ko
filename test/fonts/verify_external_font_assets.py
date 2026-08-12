#!/usr/bin/env python3
"""외부 글꼴 assets 이미지의 형식과 펌웨어 연결 설정을 검사한다."""

from __future__ import annotations

import struct
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from pack_font_assets import FORMAT_VERSION, HEADER_SIZE, MAGIC, build_asset_image, fnv1a


def main() -> int:
    failures: list[str] = []
    with tempfile.TemporaryDirectory() as temporary_dir:
        output = Path(temporary_dir) / "assets.bin"
        total, ui_size, reader_size = build_asset_image(ROOT, output)
        image = output.read_bytes()

    fields = struct.unpack("<IHHII" + "III" * 2 + "6I", image[:HEADER_SIZE])
    magic, version, header_size, encoded_total, payload_checksum = fields[:5]
    ui_offset, encoded_ui_size, ui_checksum = fields[5:8]
    reader_offset, encoded_reader_size, reader_checksum = fields[8:11]

    if (magic, version, header_size, encoded_total) != (MAGIC, FORMAT_VERSION, HEADER_SIZE, total):
        failures.append("assets header identity or size fields are invalid")
    if encoded_ui_size != ui_size or encoded_reader_size != reader_size:
        failures.append("assets record sizes do not match extracted bitmaps")
    if reader_offset != ui_offset + encoded_ui_size or total != reader_offset + encoded_reader_size:
        failures.append("assets records are not tightly packed")
    if fnv1a(image[HEADER_SIZE:]) != payload_checksum:
        failures.append("assets payload checksum mismatch")
    if fnv1a(image[ui_offset : ui_offset + encoded_ui_size]) != ui_checksum:
        failures.append("UI font checksum mismatch")
    if fnv1a(image[reader_offset : reader_offset + encoded_reader_size]) != reader_checksum:
        failures.append("reader font checksum mismatch")

    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8-sig")
    manager_header = (ROOT / "src" / "FontAssetManager.h").read_text(encoding="utf-8")
    if "-DCROSSPOINT_EXTERNAL_FONT_ASSETS=1" not in platformio:
        failures.append("external font assets build flag is missing")
    if "post:scripts/build_font_assets.py" not in platformio:
        failures.append("font assets PlatformIO post-action is missing")
    if "CACHE_SLOT_COUNT = 1" not in manager_header:
        failures.append("font bitmap cache should stay within one 4 KiB slot")

    if failures:
        print("FAIL: External font assets configuration is invalid.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print(f"PASS: External font assets are valid ({total:,} bytes).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
