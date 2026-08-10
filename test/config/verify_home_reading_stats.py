from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
RECENT_BOOKS = ROOT / "src" / "RecentBooksStore.h"
HOME = ROOT / "src" / "activities" / "home" / "HomeActivity.cpp"
RECENT_BOOKS_ACTIVITY = ROOT / "src" / "activities" / "home" / "RecentBooksActivity.cpp"
READING_PROGRESS = ROOT / "src" / "RecentBookProgress.cpp"
LYRA = ROOT / "src" / "components" / "themes" / "lyra" / "LyraTheme.cpp"
ENGLISH = ROOT / "lib" / "I18n" / "translations" / "english.yaml"
KOREAN = ROOT / "lib" / "I18n" / "translations" / "korean.yaml"


def require_tokens(path: Path, tokens: tuple[str, ...]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return [token for token in tokens if token not in text]


def main() -> int:
    checks = {
        RECENT_BOOKS: (
            "bool hasBookProgress = false;",
            "uint8_t bookProgress = 0;",
            "bool hasChapterProgress = false;",
            "uint8_t chapterProgress = 0;",
            "std::string currentChapter;",
        ),
        READING_PROGRESS: (
            "void loadEpubProgress(RecentBook& book)",
            "void loadXtcProgress(RecentBook& book)",
            "void loadTxtProgress(RecentBook& book)",
            '"/progress.bin"',
            '"/index.bin"',
        ),
        HOME: ("RecentBookProgress::load(recentBooks.back());",),
        RECENT_BOOKS_ACTIVITY: (
            "RecentBookProgress::load(recentBooks.back());",
            'std::to_string(book.bookProgress) + "%"',
        ),
        LYRA: (
            "void drawHomeProgress(",
            "tr(STR_BOOK_PROGRESS)",
            "tr(STR_CHAPTER_PROGRESS)",
            "tr(STR_CURRENT_CHAPTER)",
        ),
        ENGLISH: (
            'STR_BOOK_PROGRESS: "Book Progress"',
            'STR_CHAPTER_PROGRESS: "Chapter Progress"',
            'STR_CURRENT_CHAPTER: "Current Chapter"',
        ),
        KOREAN: (
            'STR_BOOK_PROGRESS: "책 진행률"',
            'STR_CHAPTER_PROGRESS: "챕터 진행률"',
            'STR_CURRENT_CHAPTER: "현재 챕터"',
        ),
    }

    failures = []
    for path, tokens in checks.items():
        missing = require_tokens(path, tokens)
        if missing:
            failures.append(f"{path.relative_to(ROOT)}: {missing}")

    if failures:
        print("홈 독서 진행률 연결 요소가 누락되었습니다.", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("확인 완료: 홈·최근 도서 진행률 데이터·UI·번역 연결")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
