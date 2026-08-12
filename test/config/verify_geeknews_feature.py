#!/usr/bin/env python3
"""GeekNews 목록, Markdown 본문, 홈 메뉴 및 네트워크 연결을 검증한다."""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]


def require(text: str, token: str, description: str) -> None:
    if token not in text:
        raise AssertionError(f"{description}: {token!r}")


def main() -> int:
    platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
    size_guard = (ROOT / "scripts/check_firmware_size.py").read_text(encoding="utf-8")
    home = (ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
    manager = (ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8")
    activity_h = (ROOT / "src/activities/news/GeekNewsActivity.h").read_text(encoding="utf-8")
    activity = (ROOT / "src/activities/news/GeekNewsActivity.cpp").read_text(encoding="utf-8")

    if "-<network/HttpDownloader.cpp>" not in platformio:
        raise AssertionError("사용하지 않는 범용 HTTP 다운로드 모듈이 빌드에 포함되어 있습니다")
    require(platformio, "post:scripts/check_firmware_size.py", "8 MiB 부팅 한도 검사가 빌드에 연결되지 않음")
    require(platformio, "board_upload.maximum_size = 8388608", "PlatformIO 앱 크기 한도가 8 MiB가 아님")
    require(platformio, "-UENABLE_SERIAL_LOG", "default 빌드가 직렬 로그로 이미지 한도를 초과할 수 있음")
    require(size_guard, "8 * 1024 * 1024", "ESP32-C3 앱 이미지 한도 값이 없음")
    require(size_guard, "raise RuntimeError", "앱 이미지 한도 초과 시 빌드를 차단하지 않음")
    require(home, "tr(STR_GEEKNEWS)", "홈 화면에 GeekNews 메뉴가 없음")
    require(home, "onGeekNewsOpen()", "홈 화면 GeekNews 진입점이 없음")
    require(manager, "std::make_unique<GeekNewsActivity>", "ActivityManager가 GeekNews 화면을 만들지 않음")

    require(activity, 'kHost = "news.hada.io"', "GeekNews 공식 호스트를 사용하지 않음")
    require(activity, 'kFeedPath = "/rss/news"', "공식 Atom 피드를 사용하지 않음")
    require(activity, 'kTopicPathPrefix = "/topic/"', "공식 Markdown 경로를 사용하지 않음")
    require(activity, 'path = std::string(kTopicPathPrefix) + std::to_string(topicId) + ".md"',
            "토픽 Markdown 본문을 요청하지 않음")
    require(activity, "NetworkClientSecure", "HTTPS 연결을 사용하지 않음")
    require(activity, "Accept-Encoding: identity", "압축되지 않은 응답을 요청하지 않음")
    require(activity, "chunked", "청크 전송 응답 처리가 없음")
    require(activity, "receiveFeedChunk", "Atom 피드를 스트리밍으로 처리하지 않음")
    require(activity, "kMaxFeedEntryBytes = 16 * 1024", "비정상적으로 큰 피드 항목 제한이 없음")
    require(activity, "kMaxArticleBytes = 48 * 1024", "본문 메모리 사용량 제한이 없음")
    if "std::string xml" in activity:
        raise AssertionError("Atom 피드 전체를 RAM에 적재하고 있습니다")
    require(activity_h, "kMaxTopics = 20", "최신 토픽 개수 제한이 없음")
    require(activity, "std::make_unique<WifiSelectionActivity>", "저장된 Wi-Fi 자동 연결이 없음")
    require(activity, "WiFi.mode(WIFI_OFF)", "화면 종료 시 Wi-Fi를 끄지 않음")

    for token, description in [
        ("InlineStyle::Bold", "굵게 렌더링이 없음"),
        ("InlineStyle::Italic", "기울임 렌더링이 없음"),
        ("InlineStyle::Code", "코드 렌더링이 없음"),
        ("InlineStyle::Link", "링크 렌더링이 없음"),
        ('content.rfind("> ", 0)', "인용문 처리가 없음"),
        ("content[0] == '-'", "목록 처리가 없음"),
        ('stripped == "---"', "구분선 처리가 없음"),
        ('trim(line).rfind("```", 0)', "코드 블록 처리가 없음"),
        ("rebuildPageStarts();", "페이지 나누기가 없음"),
    ]:
        require(activity, token, description)

    print("PASS: GeekNews Atom 목록, Markdown 본문, 페이지 읽기 및 Wi-Fi 연결을 확인했습니다.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
