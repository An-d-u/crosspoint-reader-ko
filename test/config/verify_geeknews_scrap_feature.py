#!/usr/bin/env python3
"""GeekNews 스크랩 저장, 목록 탐색 및 삭제 확인 흐름을 검증한다."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, token: str, description: str) -> None:
    if token not in text:
        raise AssertionError(f"{description}: {token!r}")


def main() -> int:
    header = (ROOT / "src/activities/news/GeekNewsActivity.h").read_text(encoding="utf-8")
    activity = (ROOT / "src/activities/news/GeekNewsActivity.cpp").read_text(encoding="utf-8")
    store_header = (ROOT / "src/GeekNewsScrapStore.h").read_text(encoding="utf-8")
    store = (ROOT / "src/GeekNewsScrapStore.cpp").read_text(encoding="utf-8")
    english = (ROOT / "lib/I18n/translations/english.yaml").read_text(encoding="utf-8")
    korean = (ROOT / "lib/I18n/translations/korean.yaml").read_text(encoding="utf-8")

    require(header, "View::", "GeekNews 화면 상태가 정의되지 않음")
    require(header, "Scraps, ConfirmDelete", "스크랩 목록과 삭제 확인 화면이 없음")
    require(header, "GeekNewsScrapStore scrapStore_", "스크랩 저장소가 GeekNews 화면에 연결되지 않음")

    require(activity, "openScraps();", "메인 화면에서 스크랩 목록으로 이동하지 않음")
    require(activity, "tr(STR_GEEKNEWS_SCRAPS), tr(STR_RETRY)", "메인 화면의 위 버튼이 스크랩 목록이 아님")
    require(activity, "scrapArticle();", "본문의 확인 버튼이 스크랩을 저장하지 않음")
    require(activity, "tr(STR_GEEKNEWS_SCRAP)", "본문에 스크랩 버튼이 없음")
    require(activity, "tr(STR_BACK), scraps.empty() ? \"\" : tr(STR_DELETE)",
            "스크랩 목록 버튼 순서가 뒤로/삭제가 아님")
    require(activity, "scraps.empty() ? \"\" : tr(STR_OPEN), scraps.empty() ? \"\" : \"QR\"",
            "스크랩 목록 버튼 순서가 열기/QR이 아님")
    require(activity, "openSelectedScrap();", "저장된 GeekNews 기사를 다시 열 수 없음")
    require(activity, "articleBackView_ = View::Scraps", "열어 본 기사에서 스크랩 목록으로 복귀하지 않음")
    require(activity, "std::make_unique<QrDisplayActivity>", "저장된 주소를 QR로 표시하지 않음")
    require(activity, "GeekNewsScrapStore::topicUrl(pendingTopicId_)",
            "스크랩 저장 주소가 GeekNews 요약 페이지가 아님")
    require(activity, "{MappedInputManager::Button::Down}", "측면 아래 버튼으로 목록을 이동하지 않음")
    require(activity, "{MappedInputManager::Button::Up}", "측면 위 버튼으로 목록을 이동하지 않음")
    require(activity, "tr(STR_NO), tr(STR_YES)", "삭제 확인 화면이 아니오/예 버튼을 제공하지 않음")
    require(activity, "scrapStore_.removeAt(selectedScrap_)", "삭제 확인 후 선택 항목을 삭제하지 않음")

    next_page = re.search(r"const bool nextPage =(?P<body>.*?);", activity, re.DOTALL)
    if next_page is None:
        raise AssertionError("본문 페이지 이동 조건을 찾을 수 없음")
    if "Button::Confirm" in next_page.group("body"):
        raise AssertionError("스크랩 버튼이 여전히 다음 페이지 이동도 실행함")

    require(store_header, "kMaxScraps = 20", "스크랩 개수의 메모리 상한이 없음")
    require(store_header, "kMaxTitleBytes = 192", "스크랩 제목 길이 상한이 없음")
    require(store_header, "kMaxUrlBytes = 512", "스크랩 URL 길이 상한이 없음")
    require(store, 'kStoragePath[] = "/.crosspoint/geeknews_scraps.bin"', "스크랩 영구 저장 파일이 없음")
    require(store, "AddResult::AlreadyExists", "중복 스크랩 방지가 없음")
    require(store, 'return std::string("https://news.hada.io/topic?id=") + std::to_string(topicId);',
            "GeekNews 토픽 주소 생성이 없음")
    require(store, "normalizeUrls()", "기존 원문 주소 스크랩을 GeekNews 토픽 주소로 변환하지 않음")
    require(store, "kTemporaryPath", "스크랩 저장 중 임시 파일을 사용하지 않음")
    require(store, "kBackupPath", "스크랩 저장 실패 복구용 백업이 없음")

    require(english, 'STR_GEEKNEWS_DELETE_SCRAP_CONFIRM: "Are you sure you want to delete it?"',
            "영어 삭제 확인 문구가 없음")
    require(korean, 'STR_GEEKNEWS_DELETE_SCRAP_CONFIRM: "정말 삭제하겠습니까?"',
            "한국어 삭제 확인 문구가 정확하지 않음")

    print("PASS: GeekNews 스크랩 저장, 목록 탐색 및 삭제 확인 흐름을 확인했습니다.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
