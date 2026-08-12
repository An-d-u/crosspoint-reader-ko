#!/usr/bin/env python3
"""Study 기능의 저장 형식과 화면 연결이 함께 유지되는지 검사한다."""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, needle: str, description: str) -> None:
    if needle not in text:
        raise AssertionError(f"{description}: {needle!r}")


def main() -> int:
    deck_h = (ROOT / "src/activities/study/StudyDeck.h").read_text(encoding="utf-8")
    deck_cpp = (ROOT / "src/activities/study/StudyDeck.cpp").read_text(encoding="utf-8")
    activity = (ROOT / "src/activities/study/StudyActivity.cpp").read_text(encoding="utf-8")
    credential_store = (ROOT / "src/WifiCredentialStore.h").read_text(encoding="utf-8")
    scheduler = (ROOT / "src/activities/study/StudyScheduler.cpp").read_text(encoding="utf-8")
    home = (ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
    manager = (ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8")
    web = (ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")

    require(deck_h, "kCardRecordSize = 32", "카드 레코드 크기가 바뀜")
    require(deck_cpp, "'X', 'S', 'T', 'U', 'D', 'Y', 'D'", "덱 매직이 없음")
    require(deck_cpp, "'X', 'S', 'T', 'U', 'D', 'Y', 'M'", "메타 매직이 없음")
    require(activity, "uint8_t record[32]", "리뷰 로그 레코드 크기가 바뀜")
    require(activity, "record[16] = static_cast<uint8_t>(rating)", "평가를 리뷰 로그에 쓰지 않음")
    require(activity, "record[17] = before.state", "평가 전 상태를 리뷰 로그에 쓰지 않음")
    require(activity, "cardSource_->flush()", "카드 상태를 즉시 플러시하지 않음")
    require(scheduler, "State::Relearning", "재학습 상태 처리가 없음")

    require(home, "onStudyOpen()", "홈 화면 학습 진입점이 없음")
    require(manager, "std::make_unique<StudyActivity>", "ActivityManager가 학습 화면을 만들지 않음")
    require(web, 'server->on("/api/time", HTTP_POST', "브라우저 시각 동기화 경로가 없음")
    require(web, "settimeofday", "기기 시각을 적용하지 않음")
    require(credential_store, "MAX_NETWORKS = 16", "Wi-Fi 저장 한도가 16개가 아님")
    require(activity, "std::make_unique<WifiSelectionActivity>", "학습 진입 시 Wi-Fi 연결 경로가 없음")
    require(activity, 'esp_sntp_setservername(0, "pool.ntp.org")', "NTP 시간 동기화가 없음")
    require(activity, "view_ = View::SyncingTime", "시간 동기화 화면 상태가 없음")
    require(activity, "WiFi.mode(WIFI_OFF)", "시간 동기화 후 Wi-Fi를 끄지 않음")
    require(activity, "openSelectedDeck();", "동기화 실패 시 오프라인 학습으로 진행하지 않음")
    require(activity, "parseStudyText", "Anki 후리가나 표기를 해석하지 않음")
    require(activity, "enrichHeadwordRuby", "예문의 읽기를 표제어 한자에 보완하지 않음")
    require(activity, "wrapStudyText", "공백 없는 일본어 예문을 글자 단위로 줄바꿈하지 않음")
    require(activity, "contentBottom - readingReserve - meaningReserve", "예문이 남은 화면 높이를 활용하지 않음")

    print("PASS: Study 덱, 스케줄, 저장, 홈 진입 및 시각 동기화 연결이 확인됐습니다.")
    require(activity, "token.width = baseWidth", "학습 본문 폭이 원문 폭만 따르지 않음")
    require(activity, "StudyRubyRun", "후리가나를 별도로 배치하는 오버레이 구조가 없음")
    require(activity, "resolveStudyRubyOverlaps", "서로 겹치는 후리가나 보정이 없음")
    require(activity, "const int preferredX = x + (baseWidth - rubyWidth) / 2", "후리가나가 원문 중앙에 배치되지 않음")
    if "token.width = std::max(baseWidth, rubyWidth)" in activity:
        raise AssertionError("후리가나 폭이 원문 흐름을 밀어내고 있음")
    require(activity, "kStudyRubyGapPx = 3", "인접한 후리가나 사이의 최소 간격이 없음")
    require(activity, "static_cast<int64_t>(previous.blockPositionSum) * current.blockSize",
            "후리가나 위치를 방향 편향 없이 병합하는 제약 계산이 없음")
    require(activity, "balancedPosition + runs[index].x",
            "후리가나 순서와 최소 간격을 보존하는 배치가 없음")
    require(activity, "rubyRuns.reserve(rubyCount)", "후리가나 작업 메모리를 실제 후리가나 수만큼 예약하지 않음")
    require(activity, "requiredWidth <= availableWidth", "후리가나 배치가 화면 경계를 확인하지 않음")
    if "StudyRubyGroup" in activity or "placeStudyRubyRunsGlobally" in activity:
        raise AssertionError("한쪽 쏠림을 만들던 휴리스틱 그룹 배치가 남아 있음")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}")
        sys.exit(1)
