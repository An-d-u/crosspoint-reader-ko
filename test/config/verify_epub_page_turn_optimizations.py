#!/usr/bin/env python3
"""EPUB 페이지 전환 최적화의 핵심 연결이 유지되는지 검사한다."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, token: str, description: str) -> None:
    if token not in text:
        raise AssertionError(f"{description}: {token!r}")


def main() -> int:
    font_cache = (ROOT / "lib/GfxRenderer/FontCacheManager.cpp").read_text(encoding="utf-8")
    section = (ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
    activity_h = (ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
    activity_cpp = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")

    if "scanText_.shrink_to_fit()" in font_cache:
        raise AssertionError("글꼴 스캔 버퍼가 페이지마다 다시 해제됩니다")

    require(section, "bool Section::ensureSectionFileOpen()", "섹션 캐시 파일 재사용 함수가 없습니다")
    require(section, "return Page::deserialize(file);", "열린 섹션 파일에서 페이지를 읽지 않습니다")
    require(activity_h, "PAGE_CACHE_SIZE = 3", "이전·현재·다음 페이지 캐시 크기가 다릅니다")
    require(activity_cpp, "loadCachedPage(section->currentPage, true)", "현재 페이지가 캐시를 사용하지 않습니다")
    require(activity_cpp, "prefetchAdjacentPages();", "인접 페이지를 미리 읽지 않습니다")
    require(activity_cpp, "currentPageFootnotes = page->footnotes", "캐시된 페이지의 각주가 소모됩니다")

    require(activity_cpp, "progressSaveDelayMs = 750", "진행률 저장 지연이 없습니다")
    require(activity_cpp, "queueProgressSave(currentSpineIndex", "페이지 표시 경로가 진행률 저장을 예약하지 않습니다")
    require(activity_cpp, "flushPendingProgress();\n  clearPageCache();", "종료 전에 진행률을 저장하지 않습니다")

    print("PASS: EPUB 페이지 캐시, 파일 재사용, 지연 저장 연결을 확인했습니다.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
