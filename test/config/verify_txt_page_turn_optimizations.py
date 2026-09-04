#!/usr/bin/env python3
"""TXT 페이지 전환 최적화의 핵심 연결이 유지되는지 검증한다."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, token: str, description: str) -> None:
    if token not in text:
        raise AssertionError(f"{description}: {token!r}")


def main() -> int:
    activity_h = (ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
    activity_cpp = (ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")

    require(activity_h, "PAGE_CACHE_SIZE = 3", "이전·현재·다음 페이지 캐시 크기가 올바르지 않습니다")
    require(activity_cpp, "loadCachedPage(currentPage, true)", "현재 페이지가 캐시를 사용하지 않습니다")
    require(activity_cpp, "prefetchAdjacentPages();", "인접 페이지를 미리 읽지 않습니다")

    require(activity_h, "FsFile pageFile", "페이지 캐시 파일을 재사용하지 않습니다")
    require(activity_cpp, "Page::deserialize(pageFile)", "완성된 EPUB 페이지 캐시를 사용하지 않습니다")
    page_load = activity_cpp.split("const Page* TxtReaderActivity::loadCachedPage", 1)[1].split(
        "void TxtReaderActivity::prefetchAdjacentPages", 1
    )[0]
    if "Converter" in page_load or "parseAndBuildPages" in page_load:
        raise AssertionError("페이지를 넘길 때마다 원문을 다시 변환합니다")

    require(activity_cpp, "PROGRESS_SAVE_DELAY_MS = 750", "진행률 저장 지연이 없습니다")
    require(activity_cpp, "queueProgressSave(currentPage);", "페이지 표시 경로가 저장을 예약하지 않습니다")
    require(
        activity_cpp,
        "flushPendingProgress();\n  clearPageCache();",
        "리더 종료 전에 대기 중인 진행률을 저장하지 않습니다",
    )

    print("PASS: TXT·MD 페이지 캐시, 파일 핸들 재사용, 지연 저장 연결을 확인했습니다.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
