#!/usr/bin/env python3
"""피드 선택, GeekNews/Hacker News 목록·본문 및 네트워크 연결을 검증한다."""

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
    client = (ROOT / "src/network/FeedSecureClient.h").read_text(encoding="utf-8")

    if "-<network/HttpDownloader.cpp>" not in platformio:
        raise AssertionError("사용하지 않는 범용 HTTP 다운로드 모듈이 빌드에 포함되어 있습니다")
    require(platformio, "post:scripts/check_firmware_size.py", "8 MiB 부팅 한도 검사가 빌드에 연결되지 않음")
    require(platformio, "board_upload.maximum_size = 8388608", "PlatformIO 앱 크기 한도가 8 MiB가 아님")
    require(platformio, "-UENABLE_SERIAL_LOG", "default 빌드가 직렬 로그로 이미지 한도를 초과할 수 있음")
    require(size_guard, "8 * 1024 * 1024", "ESP32-C3 앱 이미지 한도 값이 없음")
    require(size_guard, "raise RuntimeError", "앱 이미지 한도 초과 시 빌드를 차단하지 않음")
    require(home, "tr(STR_FEEDS)", "홈 화면에 피드 메뉴가 없음")
    require(home, "onFeedOpen()", "홈 화면 피드 진입점이 없음")
    require(manager, "goToFeed()", "ActivityManager에 피드 진입점이 없음")
    require(manager, "std::make_unique<GeekNewsActivity>", "ActivityManager가 피드 화면을 만들지 않음")

    require(activity_h, "View::", "피드 화면 상태가 정의되지 않음")
    require(activity_h, "Sources", "피드 소스 선택 화면이 없음")
    require(activity_h, "FeedSource", "피드 소스 구분이 없음")
    require(activity, "drawSources()", "GeekNews와 Hacker News 선택 화면이 없음")

    require(activity, 'kHost = "news.hada.io"', "GeekNews 공식 호스트를 사용하지 않음")
    require(activity, 'kFeedPath = "/rss/news"', "공식 Atom 피드를 사용하지 않음")
    require(activity, '"/https://news.hada.io/topic?id="', "공개 GeekNews 토픽 페이지를 요청하지 않음")
    if ' + ".md"' in activity:
        raise AssertionError("폐기된 GeekNews Markdown 경로를 계속 사용하고 있습니다")
    require(client, "public NetworkClientSecure", "HTTPS 연결을 사용하지 않음")
    require(activity, "FeedSecureClient client", "TLS의 일시적인 읽기 실패를 보완하지 않음")
    require(client, "readFeedBytes(*this", "검증된 대기 함수를 사용하지 않음")
    require(activity, "lastDiagnostics_.received", "수신량 진단이 화면에 표시되지 않음")
    require(activity, "!diagnostics.timedOut", "시간 초과 응답을 정상으로 처리할 수 있음")
    require(activity, "HTTPClient", "검증된 HTTP 응답 처리기를 사용하지 않음")
    require(activity, 'setAcceptEncoding("identity")', "압축되지 않은 응답을 요청하지 않음")
    require(activity, "http.writeToStream", "HTTP 라이브러리의 청크 응답 처리를 사용하지 않음")
    if "readStringUntil" in activity or "client.readBytes" in activity:
        raise AssertionError("취약한 자체 HTTP/1.1 청크 파서가 남아 있습니다")
    require(activity, "receiveFeedChunk", "Atom 피드를 스트리밍으로 처리하지 않음")
    require(activity, "kMaxFeedEntryBytes = 16 * 1024", "비정상적으로 큰 피드 항목 제한이 없음")
    require(activity, "kMaxArticleBytes = 48 * 1024", "본문 메모리 사용량 제한이 없음")
    require(activity, "const std::string_view body", "본문 전체를 중복 할당하고 있음")
    require(activity, "receiveGeekArticleChunk", "GeekNews 본문을 스트리밍으로 분리하지 않음")
    require(activity, 'line == "## 댓글과 토론"', "사용하지 않는 GeekNews 댓글을 저장하고 있음")
    require(activity, "ensureLayoutCapacity(articleSpans_", "span 저장소를 안전하게 확장하지 않음")
    require(activity, "ensureLayoutCapacity(articleText_", "본문 문자열의 연속 저장소를 안전하게 확장하지 않음")
    require(activity, "kMaxLayoutLines = 8192", "비정상적으로 긴 본문의 레이아웃 상한이 없음")
    require(activity, "kMaxLayoutSpans = 8192", "스타일 구간의 상한이 없음")
    require(activity, "kWifiConnectTimeoutMs = 15000", "Wi-Fi 재연결 대기 시간이 너무 짧음")
    require(activity, "WIFI_STORE.findCredential(ssid)", "저장된 Wi-Fi 비밀번호로 재연결하지 않음")
    require(activity, "wifiNetworkReady", "Wi-Fi 주소 설정 완료 여부를 확인하지 않음")
    require(activity, "WiFi.gatewayIP()", "게이트웨이 준비 상태를 확인하지 않음")
    require(activity, "WiFi.dnsIP()", "DNS 서버 준비 상태를 확인하지 않음")
    require(activity, "kNetworkSettleMs", "Wi-Fi 재연결 직후 안정화 대기가 없음")
    require(activity, "classifyConnectionFailure", "연결 실패 원인 진단이 없음")
    require(activity, "WiFi.hostByName(host, address)", "연결 실패 후 DNS 상태를 구분하지 않음")
    require(activity, "kExtractorTimeoutMs = 60000", "느린 본문 변환 서버의 60초 대기가 없음")
    require(activity, "kMaxRedirects = 5", "리다이렉트 횟수 제한이 없음")
    require(activity, "HTTPC_STRICT_FOLLOW_REDIRECTS", "HTTP 리다이렉트를 따라가지 않음")
    require(activity, "http.setRedirectLimit(kMaxRedirects)", "리다이렉트 제한이 적용되지 않음")
    require(activity, "FeedLoadError::Dns", "DNS 실패 원인 구분이 없음")
    require(activity, "FeedLoadError::Tls", "TLS 실패 원인 구분이 없음")
    require(activity, "FeedLoadError::Response", "응답 수신 실패 원인 구분이 없음")
    require(activity, "loadErrorMessage()", "실패 원인을 화면에 표시하지 않음")
    require(activity, "heap_caps_get_largest_free_block", "대형 본문 할당 전 연속 메모리를 확인하지 않음")
    require(activity, "kNetworkAttempts = 3", "일시적인 HTTPS 실패 자동 재시도가 없음")
    require(activity, "waitForWifi()", "요청 전 Wi-Fi 연결 확인이 없음")
    require(activity, "renderer.getTextAdvanceX", "Markdown span을 실제 진행 폭으로 배치하지 않음")
    require(activity, "kTextRightSafetyPx", "합성 굵게의 우측 안전 여백이 없음")
    require(activity, "MappedInputManager::Button::PageBack", "우측 위 버튼으로 이전 페이지를 열 수 없음")
    require(activity, "MappedInputManager::Button::PageForward", "우측 아래 버튼으로 다음 페이지를 열 수 없음")
    if "std::string xml" in activity:
        raise AssertionError("Atom 피드 전체를 RAM에 적재하고 있습니다")
    require(activity_h, "kMaxTopics = 20", "최신 토픽 개수 제한이 없음")
    require(activity, "std::make_unique<WifiSelectionActivity>", "저장된 Wi-Fi 자동 연결이 없음")
    require(activity, "WiFi.mode(WIFI_OFF)", "화면 종료 시 Wi-Fi를 끄지 않음")

    require(activity, 'kHackerNewsHost = "hn.algolia.com"', "Hacker News 목록 API 호스트가 없음")
    require(activity, 'tags=front_page&hitsPerPage=20', "Hacker News 전면 목록을 한 번에 요청하지 않음")
    require(activity, 'kExtractorHost = "r.jina.ai"', "외부 기사 Markdown 추출 경로가 없음")
    require(activity, "kHackerNewsTempPath", "Hacker News 목록을 TLS 버퍼와 분리해 저장하지 않음")
    require(activity, "kArticleTempPath", "본문을 TLS 버퍼와 분리하는 임시 저장소가 없음")
    require(activity, "DeserializationOption::Filter", "Hacker News JSON 필드 필터가 없음")
    require(activity, "kMaxHackerNewsFeedBytes", "Hacker News 목록 응답 크기 제한이 없음")
    require(activity, "FileResponseContext context{&output, 0, kMaxArticleBytes}",
            "Hacker News 본문 다운로드 크기 제한이 없음")
    require(activity, "hackerNewsHtmlToMarkdown", "Hacker News 자체 글 HTML 처리가 없음")
    require(activity, '"https://news.ycombinator.com/item?id="', "Hacker News 자체 글 원문 주소가 없음")
    require(activity, "showArticleQr()", "Hacker News 원문 QR 열기가 없음")

    for token, description in [
        ("InlineStyle::Bold", "굵게 렌더링이 없음"),
        ("InlineStyle::Italic", "기울임 렌더링이 없음"),
        ("InlineStyle::Code", "코드 렌더링이 없음"),
        ("InlineStyle::Link", "링크 렌더링이 없음"),
        ('content.rfind("> ", 0)', "인용문 처리가 없음"),
        ("content[0] == '-'", "목록 처리가 없음"),
        ('stripped == "---"', "구분선 처리가 없음"),
        ('trim(line).rfind("```", 0)', "코드 블록 처리가 없음"),
        ("rebuildPageStarts();", "RAM 레이아웃의 페이지 구분이 없음"),
    ]:
        require(activity, token, description)

    print("PASS: 피드 선택, GeekNews/Hacker News 목록·본문, 페이지 읽기 및 Wi-Fi 연결을 확인했습니다.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
