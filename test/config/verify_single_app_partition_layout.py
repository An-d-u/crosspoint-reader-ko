#!/usr/bin/env python3
"""
단일 앱 슬롯 파티션 레이아웃이 적용되었는지 검사한다.
USB 직플래시 전제를 기준으로 OTA 듀얼 슬롯 대신 factory 단일 슬롯을 사용한다.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PARTITIONS_CSV = ROOT / "partitions.csv"
FLASH_SIZE = 0x1000000
MIN_EXPECTED_APP_SIZE = 0xD00000


def main() -> int:
    failures: list[str] = []
    rows: list[list[str]] = []

    with PARTITIONS_CSV.open("r", encoding="utf-8", newline="") as handle:
        for raw_row in csv.reader(handle):
            if not raw_row:
                continue
            first = raw_row[0].strip()
            if not first or first.startswith("#"):
                continue
            row = [item.strip() for item in raw_row]
            rows.append(row)

    app_rows = [row for row in rows if len(row) >= 5 and row[1] == "app"]
    if len(app_rows) != 1:
        failures.append(f"Expected exactly one app partition, found {len(app_rows)}")
    else:
        app = app_rows[0]
        if app[0] != "app0":
            failures.append(f"Single app partition should be named app0, found {app[0]}")
        if app[2] != "factory":
            failures.append(f"Single app partition should use subtype factory, found {app[2]}")
        size = int(app[4], 16)
        if size < MIN_EXPECTED_APP_SIZE:
            failures.append(
                f"Single app partition should be at least 0x{MIN_EXPECTED_APP_SIZE:X}, found 0x{size:X}"
            )

    if any(row[0] == "app1" for row in rows):
        failures.append("partitions.csv should not define app1 in single-slot mode")

    if any(row[0] == "otadata" for row in rows):
        failures.append("partitions.csv should not define otadata in single-slot mode")

    end_offsets = []
    for row in rows:
        if len(row) < 5:
            continue
        offset = int(row[3], 16)
        size = int(row[4], 16)
        end_offsets.append(offset + size)

    if end_offsets and max(end_offsets) > FLASH_SIZE:
        failures.append(
            f"Partition layout exceeds flash size: end=0x{max(end_offsets):X}, flash=0x{FLASH_SIZE:X}"
        )

    if failures:
        print("FAIL: Single app partition layout is invalid.")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print("PASS: Single app partition layout is applied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
