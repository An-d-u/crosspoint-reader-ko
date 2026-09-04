#!/usr/bin/env python3
"""피드 다운로드와 화면 데이터의 메모리 수명 및 실패 진단 연결을 검사한다."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/activities/news/GeekNewsActivity.cpp").read_text(encoding="utf-8")


def function(signature: str) -> str:
    start = SOURCE.index(signature)
    return SOURCE[start:SOURCE.index("\n}", start) + 2]


def main() -> None:
    release = function("void GeekNewsActivity::releaseArticleLayout()")
    for member in ("articleLines_", "articleSpans_", "articleText_", "pageStarts_"):
        assert f".swap({member})" in release, f"이전 화면의 용량 미반환: {member}"
    for signature, following in (
        ("void GeekNewsActivity::loadTopics()", "loadHackerNewsTopics()"),
        ("void GeekNewsActivity::loadArticle(", "loadHackerNewsArticle(topicId)"),
    ):
        body = function(signature)
        assert body.index("releaseArticleLayout();") < body.index(following)

    # 렌더링 스레드가 이전 화면을 읽는 동안 버퍼를 해제하지 않아야 한다.
    loop = function("void GeekNewsActivity::loop()")
    assert loop.index("requestUpdateAndWait();") < loop.index("loadArticle(pendingTopicId_)")
    read = function("bool readArticleFile(")
    assert read.index("releaseArticleWifi();") < read.index("hasAllocationRoom(length + 1)")
    wifi = function("void releaseArticleWifi()")
    assert wifi.index("WIFI_STORE.findCredential") < wifi.index("WiFi.disconnect(false)")
    assert wifi.index("WiFi.getMode() == WIFI_OFF") < wifi.index("WiFi.disconnect(false)")

    for signature in ("bool fetchGeekNewsArticle(", "bool GeekNewsActivity::loadHackerNewsArticle("):
        body = function(signature)
        assert body.index("responseReady") < body.index("readArticleFile(")
        assert body.index(".close()") < body.index("readArticleFile(")
    assert "readArticleFile(output, error, diagnostics)" in SOURCE
    assert "readArticleFile(markdown, lastError_, lastDiagnostics_)" in SOURCE

    for stage in ("ArticleBuffer", "LineBuffer", "SpanBuffer", "TextBuffer", "PageBuffer", "LineLimit", "SpanLimit", "TextLimit"):
        assert f"MemoryStage::{stage}" in SOURCE
        assert f"case FeedLoadDiagnostics::MemoryStage::{stage}:" in SOURCE
    assert SOURCE.count("layoutOverflow_ = true;") == 1, "실패 단계 기록을 우회하는 경로가 있음"
    assert "heap_caps_get_largest_free_block" in function("void recordMemoryFailure(")
    assert "kMaxArticleBytes = 48 * 1024" in SOURCE
    assert "kMaxLayoutLines = 512" not in SOURCE, "기사 전체 512줄 제한이 남아 있음"
    store = function("bool GeekNewsActivity::storeLayoutLine(")
    for member in ("articleLines_", "articleSpans_", "articleText_"):
        assert f"ensureLayoutCapacity({member}," in store
    assert store.index("ensureLayoutCapacity(articleText_,") < store.index("articleText_.insert")
    assert store.index("ensureLayoutCapacity(articleLines_,") < store.index("articleLines_.push_back")
    layout = function("void GeekNewsActivity::layoutMarkdown(")
    assert "rebuildPageStarts();" in layout
    assert ".reserve(" not in layout, "전체 레이아웃의 선할당이 남아 있음"
    pages = function("void GeekNewsActivity::rebuildPageStarts()")
    assert pages.index("ensureLayoutCapacity(pageStarts_,") < pages.index("pageStarts_.push_back")
    assert "--currentPage_;" in loop and "++currentPage_;" in loop
    for removed in ("FeedPageStorage", "flushLayoutPage", "loadCachedPage", "layoutFile_", "feed_layout.tmp"):
        assert removed not in SOURCE, f"SD 화면 저장 코드 잔존: {removed}"
    assert "articleText_.data() + span.textOffset" in SOURCE
    assert "FeedLayoutCapacity::ensure(storage, required, limit, step, hasAllocationRoom)" in SOURCE
    assert "releaseArticleLayout();" in function("void GeekNewsActivity::onExit()")
    assert "limit ? FeedLoadError::LayoutLimit : error" in function("void GeekNewsActivity::failLayout(")
    assert "if (layoutOverflow_) lastError_ = FeedLoadError::Memory;" not in SOURCE
    print("PASS: 피드 이전 화면 해제, 다운로드 후 Wi-Fi 반환, 재연결 보호 및 메모리 진단 연결")


if __name__ == "__main__":
    main()
