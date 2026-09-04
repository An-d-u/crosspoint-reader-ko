#!/usr/bin/env python3
"""개인정보가 없는 PDF 통합 검증 자료를 생성한다. 결과물은 커밋하지 않는다."""

from pathlib import Path
import argparse

from PIL import Image, ImageDraw
from pypdf import PdfReader, PdfWriter
from reportlab import rl_config
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader


def make(output: Path, font: Path, japanese_font: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    rl_config.useA85 = False

    def document(name: str):
        result = canvas.Canvas(str(output / name), pagesize=(480, 640), pageCompression=1, invariant=1)
        result.setAuthor("")
        result.setTitle("Synthetic reader fixture")
        return result

    book = document("latin.pdf")
    for page in range(1, 4):
        book.setFont("Helvetica", 18)
        book.drawString(36, 590, f"Synthetic page {page}")
        book.setFont("Helvetica", 12)
        for line in range(1, 16):
            book.drawString(36, 560 - line * 25, f"Line {line}: A quiet garden has many colorful flowers.")
        book.showPage()
    book.save()

    pdfmetrics.registerFont(TTFont("FixtureUnicode", str(font)))
    pdfmetrics.registerFont(TTFont("FixtureJapanese", str(japanese_font)))
    book = document("unicode.pdf")
    book.setFont("FixtureUnicode", 18)
    book.drawString(36, 575, "다국어 텍스트 검증")
    book.setFont("FixtureUnicode", 14)
    book.drawString(36, 525, "작은 숲에서 책을 읽습니다.")
    book.setFont("FixtureJapanese", 14)
    book.drawString(36, 490, "静かな森で本を読みます。")
    book.setFont("FixtureUnicode", 14)
    book.drawString(36, 455, "A small library beside the garden.")
    book.save()

    scan = Image.new("RGB", (800, 320), "white")
    draw = ImageDraw.Draw(scan)
    draw.text((40, 60), "SYNTHETIC OCR SAMPLE", fill="black", font_size=40)
    draw.text((40, 140), "A quiet garden beside a small library.", fill="black", font_size=27)
    for filename, with_ocr in (("ocr.pdf", True), ("image-only.pdf", False)):
        book = document(filename)
        book.drawImage(ImageReader(scan), 30, 400, width=420, height=168)
        if with_ocr:
            text = book.beginText(50, 515)
            text.setFont("Helvetica", 18)
            text.setTextRenderMode(3)
            text.textLine("SYNTHETIC OCR SAMPLE")
            text.textLine("A quiet garden beside a small library.")
            book.drawText(text)
        book.save()

    writer = PdfWriter()
    for page in PdfReader(output / "latin.pdf").pages:
        writer.add_page(page)
    writer.encrypt("fixture-password")
    writer.write(output / "encrypted.pdf")

    rl_config.useA85 = True
    book = document("unsupported-filter.pdf")
    book.drawString(36, 575, "Unsupported filter fixture")
    book.save()
    rl_config.useA85 = False

    data = bytearray((output / "latin.pdf").read_bytes())
    first = data.index(b"stream\n") + len(b"stream\n")
    end = data.index(b"endstream", first)
    data[end - 1] ^= 1
    (output / "damaged-stream.pdf").write_bytes(data)

    for filename in ("latin.pdf", "unicode.pdf", "ocr.pdf"):
        reader = PdfReader(output / filename)
        text = "\n".join(page.extract_text() or "" for page in reader.pages)
        (output / (filename + ".expected.txt")).write_text(text, encoding="utf-8")
    print(f"합성 PDF 7개 생성: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("tmp/pdfs/reader"))
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--japanese-font", type=Path, required=True)
    args = parser.parse_args()
    make(args.output, args.font, args.japanese_font)
