#!/usr/bin/env python3
"""실제 C++ 수신 및 RAM 용량 확장 함수의 정적 단언 테스트를 컴파일하여 실행한다."""

from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        candidates = list((Path.home() / ".platformio/packages/toolchain-riscv32-esp/bin").glob("*g++.exe"))
        if candidates:
            compiler = str(candidates[0])
    if not compiler:
        raise RuntimeError("C++20 컴파일러를 찾지 못했습니다")
    for source in ("FeedReadBytesTest.cpp", "FeedLayoutCapacityTest.cpp"):
        subprocess.run(
            [compiler, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "src"), str(ROOT / "test/network" / source)],
            check=True,
        )
    print("PASS: 분할 수신, 1300줄·2600구간 RAM 확장, 내용 보존, 메모리 부족·상한 경계 C++ 검증")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
