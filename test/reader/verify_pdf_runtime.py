#!/usr/bin/env python3
"""실제 PDF 추출기·uzlib을 WebAssembly로 실행하여 합성 PDF와 대조한다."""

from pathlib import Path
import argparse
import hashlib
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main(emsdk: Path, fixtures: Path) -> None:
    build = ROOT / ".cache/pdf-host"
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, EM_CONFIG=str(emsdk / ".emscripten"))
    python = emsdk / "python/python.exe"
    emcc = emsdk / "emscripten/emcc.py"
    emcpp = emsdk / "emscripten/em++.py"
    subprocess.run([str(python), str(emcc), "-O1", "-Ilib/uzlib/src", "-c", "lib/uzlib/src/tinflate.c",
                    "-o", str(build / "tinflate.o")], cwd=ROOT, env=env, check=True)
    subprocess.run([str(python), str(emcpp), "-std=c++20", "-O1", "-fno-exceptions",
                    "-sNODERAWFS=1", "-sENVIRONMENT=node", "-sALLOW_MEMORY_GROWTH=1",
                    "-sSTACK_SIZE=8192", "-sSTACK_OVERFLOW_CHECK=2",
                    "-Itest/reader/pdf_host", "-Ilib/PdfText", "-Ilib/InflateReader", "-Ilib/uzlib/src",
                    "test/reader/pdf_host/main.cpp", "lib/PdfText/PdfText.cpp", "lib/InflateReader/InflateReader.cpp",
                    str(build / "tinflate.o"), "-o", str(build / "pdf_host.cjs")], cwd=ROOT, env=env, check=True)
    node = shutil.which("node") or str(emsdk / "node/node.exe")
    for name, expected in (("latin", 0), ("unicode", 0), ("ocr", 0), ("image-only", 6),
                           ("encrypted", 4), ("unsupported-filter", 3), ("damaged-stream", 2)):
        source = fixtures / f"{name}.pdf"
        target = fixtures / f"{name}.actual.txt"
        before = hashlib.sha256(source.read_bytes()).digest()
        result = subprocess.run([node, str(build / "pdf_host.cjs"), str(source), str(target), str(fixtures)],
                                cwd=ROOT, capture_output=True, text=True, timeout=30)
        assert result.returncode == expected, f"{name}: {result.returncode}\n{result.stdout}\n{result.stderr}"
        assert hashlib.sha256(source.read_bytes()).digest() == before, f"원본 변경: {name}"
        assert not (fixtures / "pdf-content.tmp").exists() and not (fixtures / "pdf-cmap.tmp").exists()
        if expected == 0:
            actual = target.read_text(encoding="utf-8")
            reference = (fixtures / f"{name}.pdf.expected.txt").read_text(encoding="utf-8")
            assert actual.split() == reference.split(), f"추출 내용 불일치: {name}"
        else:
            assert not target.exists(), f"오류 후 불완전한 결과가 남음: {name}"
        print(f"PASS: {name}")
    print("PASS: 실제 압축 해제·PDF 추출, 8 KiB 호스트 스택, 원본 보존, 임시 파일 정리")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--emsdk", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, default=ROOT / "tmp/pdfs/reader")
    args = parser.parse_args()
    main(args.emsdk.resolve(), args.fixtures.resolve())
