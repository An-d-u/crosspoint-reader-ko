from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
STORE = ROOT / "src" / "BookmarkStore.cpp"
STORE_HEADER = ROOT / "src" / "BookmarkStore.h"
BOOKMARK_LIST = ROOT / "src" / "activities" / "reader" / "BookmarkListActivity.cpp"
EPUB_READER = ROOT / "src" / "activities" / "reader" / "EpubReaderActivity.cpp"
EPUB_MENU = ROOT / "src" / "activities" / "reader" / "EpubReaderMenuActivity.cpp"
XTC_READER = ROOT / "src" / "activities" / "reader" / "XtcReaderActivity.cpp"
TXT_READER = ROOT / "src" / "activities" / "reader" / "TxtReaderActivity.cpp"
ENGLISH = ROOT / "lib" / "I18n" / "translations" / "english.yaml"
KOREAN = ROOT / "lib" / "I18n" / "translations" / "korean.yaml"


def require_tokens(path: Path, tokens: tuple[str, ...]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return [token for token in tokens if token not in text]


def main() -> int:
    checks = {
        STORE: (
            'constexpr char bookmarkDirectory[] = "/.crosspoint/bookmarks";',
            "uint64_t stablePathHash(const std::string& path)",
            'document["version"] = bookmarkFileVersion;',
            'document["bookPath"] = bookPath;',
            'document["bookmarks"].to<JsonArray>()',
            "BookmarkStore::ToggleResult BookmarkStore::toggle",
            "bool BookmarkStore::removeAt",
        ),
        STORE_HEADER: (
            "static constexpr size_t maxBookmarks = 50;",
            "int spineIndex = -1;",
            "uint32_t pageCount = 0;",
        ),
        BOOKMARK_LIST: (
            "store.toggle(currentBookmark)",
            "confirmDeleteBookmark(bookmarkIndex)",
            "BookmarkResult{bookmark.spineIndex, bookmark.page, bookmark.pageCount}",
            "STR_HOLD_CONFIRM_DELETE",
        ),
        EPUB_READER: (
            "Bookmark EpubReaderActivity::getCurrentBookmark() const",
            "void EpubReaderActivity::openBookmarks()",
            "pendingSpineProgress",
        ),
        EPUB_MENU: ("MenuAction::BOOKMARKS", "STR_BOOKMARKS"),
        XTC_READER: (
            "DocumentReaderMenuActivity",
            "Bookmark XtcReaderActivity::getCurrentBookmark()",
            "void XtcReaderActivity::openBookmarks()",
        ),
        TXT_READER: (
            "DocumentReaderMenuActivity",
            "Bookmark TxtReaderActivity::getCurrentBookmark() const",
            "void TxtReaderActivity::openBookmarks()",
        ),
        ENGLISH: (
            'STR_BOOKMARKS: "Bookmarks"',
            'STR_ADD_BOOKMARK: "Add Bookmark"',
            'STR_REMOVE_BOOKMARK: "Remove Bookmark"',
        ),
        KOREAN: (
            'STR_BOOKMARKS: "북마크"',
            'STR_ADD_BOOKMARK: "북마크 추가"',
            'STR_REMOVE_BOOKMARK: "북마크 해제"',
        ),
    }

    failures = []
    for path, tokens in checks.items():
        missing = require_tokens(path, tokens)
        if missing:
            failures.append(f"{path.relative_to(ROOT)}: {missing}")

    store_text = STORE.read_text(encoding="utf-8")
    if "getCachePath" in store_text or '"/.crosspoint/epub"' in store_text:
        failures.append("BookmarkStore.cpp: 북마크가 리더 캐시 경로에 결합되어 있습니다.")

    if failures:
        print("북마크 기능 연결 요소가 누락되었습니다.", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("확인 완료: 북마크 저장·목록·이동·삭제·리더 연결")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
