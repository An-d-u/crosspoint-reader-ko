#!/usr/bin/env python3
"""실제 C++ 수신 함수의 정적 단언 테스트를 컴파일하여 실행한다."""

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
        raise RuntimeError("C++17 컴파일러를 찾지 못했습니다")
    subprocess.run(
        [compiler, "-std=c++17", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "src"), str(ROOT / "test/network/FeedReadBytesTest.cpp")],
        check=True,
    )
    print("PASS: 분할 수신, 기존 실패 재현, 지연, 연결 종료, 제한 시간, 시계 순환 C++ 검증")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
