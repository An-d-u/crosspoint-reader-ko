from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def verify_html_entities() -> int:
    source = (ROOT / "lib/Epub/Epub/htmlEntities.cpp").read_text(encoding="utf-8")
    pairs = re.findall(r'\{"(&[^";]+;)",\s*"([^"]*)"\}', source)
    entities = dict(pairs)
    expected = {
        "&alefsym;": "ℵ",
        "&dArr;": "⇓",
        "&hArr;": "⇔",
        "&image;": "ℑ",
        "&lArr;": "⇐",
        "&lang;": "〈",
        "&middot;": "·",
        "&rArr;": "⇒",
        "&rang;": "〉",
        "&real;": "ℜ",
        "&uArr;": "⇑",
        "&weierp;": "℘",
    }
    missing = {key: value for key, value in expected.items() if entities.get(key) != value}
    if missing:
        return fail(f"HTML 4.01 엔티티가 누락되었거나 값이 다릅니다: {missing}")

    keys = [key for key, _ in pairs]
    if keys != sorted(keys):
        return fail("HTML 엔티티 테이블이 이진 검색에 필요한 사전식 순서를 지키지 않습니다")
    return 0


def verify_css_discovery() -> int:
    epub = (ROOT / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
    zip_header = (ROOT / "lib/ZipFile/ZipFile.h").read_text(encoding="utf-8")
    fs_helpers = (ROOT / "lib/FsHelpers/FsHelpers.cpp").read_text(encoding="utf-8")

    required = (
        "void Epub::discoverCssFilesFromZip()",
        "zf.loadAllFileStatSlims()",
        "zf.enumerateFilePaths",
        "FsHelpers::hasCssExtension(filePath)",
    )
    if any(token not in epub for token in required):
        return fail("EPUB ZIP CSS 자동 탐색 흐름이 완전하지 않습니다")
    if "void enumerateFilePaths" not in zip_header or 'checkFileExtension(fileName, ".css")' not in fs_helpers:
        return fail("ZIP 경로 열거 또는 CSS 확장자 검사가 누락되었습니다")
    if epub.count("discoverCssFilesFromZip();") < 2:
        return fail("신규 캐시와 기존 캐시 경로 모두에서 CSS 자동 탐색이 호출되어야 합니다")
    return 0


def verify_footnote_path_decode() -> int:
    epub = (ROOT / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
    match = re.search(
        r"int Epub::resolveHrefToSpineIndex[\s\S]*?href\.find\('#'\)[\s\S]*?decodeUriEscapes\(rawTarget\)",
        epub,
    )
    if not match:
        return fail("각주 경로는 앵커를 먼저 분리한 뒤 URI 이스케이프를 디코딩해야 합니다")
    return 0


def verify_nested_block_styles() -> int:
    block_style = (ROOT / "lib/Epub/Epub/blocks/BlockStyle.h").read_text(encoding="utf-8")
    parser = (ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp").read_text(encoding="utf-8")
    parser_header = (ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h").read_text(encoding="utf-8")

    if "enum class CombineAxis" not in block_style or "BlockStyle addBottom" not in block_style:
        return fail("중첩 블록의 가로·세로 스타일 결합 API가 누락되었습니다")
    if "std::vector<BlockStyle> blockStyleStack" not in parser_header:
        return fail("중첩 블록 스타일 스택이 선언되지 않았습니다")
    required = (
        "blockStyleStack.push_back(accumulated)",
        "blockStyleStack.pop_back()",
        "startNewTextBlock(self->blockStyleStack.back())",
        "BlockStyle::CombineAxis::Horizontal",
        "BlockStyle::CombineAxis::Vertical",
    )
    if any(token not in parser for token in required):
        return fail("중첩 블록 스타일의 진입·복귀 흐름이 완전하지 않습니다")
    return 0


def verify_sparse_ruby_storage() -> int:
    header = (ROOT / "lib/Epub/Epub/blocks/TextBlock.h").read_text(encoding="utf-8")
    source = (ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp").read_text(encoding="utf-8")
    if "std::vector<RubyAnnotation> rubyAnnotations" not in header:
        return fail("TextBlock은 루비 정보를 희소한 RubyAnnotation 목록으로 보관해야 합니다")
    if "std::vector<std::string> rubyTexts" in header:
        return fail("TextBlock에 단어 수만큼 할당되는 루비 문자열 벡터가 다시 추가되었습니다")
    if "if (!blockHasRuby)" not in source:
        return fail("루비 없는 문장의 무할당 렌더링 경로가 누락되었습니다")
    return 0


def main() -> int:
    checks = (
        verify_html_entities,
        verify_css_discovery,
        verify_footnote_path_decode,
        verify_nested_block_styles,
        verify_sparse_ruby_storage,
    )
    for check in checks:
        result = check()
        if result != 0:
            return result
    print("확인 완료: EPUB 호환성, 중첩 스타일, 희소 루비 저장 조건")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
