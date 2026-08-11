from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
IDENTITY = ROOT / "src" / "BookDataStore.cpp"
BOOKMARKS = ROOT / "src" / "BookmarkStore.cpp"
RECENTS = ROOT / "src" / "RecentBooksStore.cpp"
READER = ROOT / "src" / "activities" / "reader" / "ReaderActivity.cpp"
EPUB = ROOT / "lib" / "Epub" / "Epub.h"
XTC = ROOT / "lib" / "Xtc" / "Xtc.h"
TXT = ROOT / "lib" / "Txt" / "Txt.cpp"


def require_tokens(path: Path, tokens: tuple[str, ...]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return [token for token in tokens if token not in text]


def main() -> int:
    checks = {
        IDENTITY: (
            'constexpr char registryPath[] = "/.crosspoint/book-data.json";',
            "bool fingerprint(const std::string& path",
            "hashRange(file, 0, size, hash)",
            "const std::array<size_t, 3> offsets",
            "cacheExists(path, oldCacheKey)",
            '"id_" + reference.id',
            "reference.previousPath = entry->path;",
        ),
        EPUB: ('cacheDir + "/epub_" + key',),
        XTC: ('cacheDir + "/xtc_" + key',),
        TXT: ('cacheBasePath + "/txt_" + key',),
        READER: (
            "BookDataStore::resolve(initialBookPath)",
            "bookData.cacheKey",
            "migrateBookmarks(initialBookPath)",
            "RECENT_BOOKS.relocateBook(bookData.previousPath, initialBookPath)",
        ),
        RECENTS: (
            "void RecentBooksStore::registerExistingBooks() const",
            "BookDataStore::resolve(book.path)",
            "bool RecentBooksStore::relocateBook",
        ),
        BOOKMARKS: (
            '"/book_id_" + bookId + ".json"',
            "loadFromFile(legacyPath, path)",
            'document["bookId"] = bookId;',
        ),
    }

    failures = []
    for path, tokens in checks.items():
        missing = require_tokens(path, tokens)
        if missing:
            failures.append(f"{path.relative_to(ROOT)}: {missing}")

    if failures:
        print("도서 ID 기반 데이터 유지 연결 요소가 누락되었습니다.", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("확인 완료: 이름·경로 변경 시 진행률·북마크·최근 도서 연결 유지")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
