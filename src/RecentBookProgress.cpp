#include "RecentBookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "RecentBooksStore.h"

namespace {
constexpr uint32_t txtCacheMagic = 0x54585449;  // "TXTI"
constexpr uint8_t txtCacheVersion = 4;
constexpr size_t txtCacheHeaderSize = 35;

uint16_t readUint16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint8_t calculatePercentage(const uint64_t current, const uint64_t total) {
  if (total == 0) {
    return 0;
  }
  return static_cast<uint8_t>(std::min<uint64_t>(100, current * 100 / total));
}

bool readProgressFile(const std::string& path, uint8_t* data, const size_t capacity, int& bytesRead) {
  FsFile file;
  if (!Storage.openFileForRead("RBP", path, file)) {
    return false;
  }
  bytesRead = file.read(data, capacity);
  file.close();
  return true;
}

void loadEpubProgress(RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  // 최근 도서 화면에서는 이미 만들어진 메타데이터 캐시만 읽고 EPUB을 다시 인덱싱하지 않습니다.
  if (!epub.load(false, true) || epub.getSpineItemsCount() <= 0) {
    return;
  }

  uint8_t data[6] = {};
  int bytesRead = 0;
  if (!readProgressFile(epub.getCachePath() + "/progress.bin", data, sizeof(data), bytesRead) ||
      (bytesRead != 4 && bytesRead != 6)) {
    return;
  }

  const int spineIndex = readUint16(data);
  if (spineIndex < 0 || spineIndex >= epub.getSpineItemsCount()) {
    return;
  }

  float chapterRatio = 0.0f;
  if (bytesRead == 6) {
    const uint16_t pageCount = readUint16(data + 4);
    if (pageCount > 0) {
      const uint16_t currentPage = std::min<uint16_t>(readUint16(data + 2), pageCount - 1);
      book.chapterProgress = calculatePercentage(static_cast<uint32_t>(currentPage) + 1, pageCount);
      book.hasChapterProgress = true;
      chapterRatio = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
    }
  }

  const float rawBookProgress = epub.calculateProgress(spineIndex, chapterRatio) * 100.0f;
  book.bookProgress = static_cast<uint8_t>(std::clamp(rawBookProgress, 0.0f, 100.0f));
  book.hasBookProgress = true;

  const int tocIndex = epub.getTocIndexForSpineIndex(spineIndex);
  if (tocIndex >= 0 && tocIndex < epub.getTocItemsCount()) {
    book.currentChapter = epub.getTocItem(tocIndex).title;
  }
}

void loadXtcProgress(RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load() || xtc.getPageCount() == 0) {
    return;
  }

  uint8_t data[4] = {};
  int bytesRead = 0;
  if (!readProgressFile(xtc.getCachePath() + "/progress.bin", data, sizeof(data), bytesRead) || bytesRead != 4) {
    return;
  }

  const uint32_t currentPage = std::min(readUint32(data), xtc.getPageCount() - 1);
  book.bookProgress = xtc.calculateProgress(currentPage);
  book.hasBookProgress = true;

  if (!xtc.hasChapters()) {
    return;
  }

  for (const auto& chapter : xtc.getChapters()) {
    if (currentPage < chapter.startPage || currentPage > chapter.endPage) {
      continue;
    }

    const uint32_t chapterPageCount = static_cast<uint32_t>(chapter.endPage) - chapter.startPage + 1;
    const uint32_t currentChapterPage = currentPage - chapter.startPage + 1;
    book.chapterProgress = calculatePercentage(currentChapterPage, chapterPageCount);
    book.hasChapterProgress = true;
    book.currentChapter = chapter.name;
    break;
  }
}

void loadTxtProgress(RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return;
  }

  uint8_t progressData[4] = {};
  int progressBytes = 0;
  if (!readProgressFile(txt.getCachePath() + "/progress.bin", progressData, sizeof(progressData), progressBytes) ||
      progressBytes != 4) {
    return;
  }

  uint8_t indexHeader[txtCacheHeaderSize] = {};
  int headerBytes = 0;
  if (!readProgressFile(txt.getCachePath() + "/index.bin", indexHeader, sizeof(indexHeader), headerBytes) ||
      headerBytes != static_cast<int>(sizeof(indexHeader)) || readUint32(indexHeader) != txtCacheMagic ||
      indexHeader[4] != txtCacheVersion || readUint32(indexHeader + 5) != txt.getFileSize()) {
    return;
  }

  const uint32_t totalPages = readUint32(indexHeader + 31);
  if (totalPages == 0) {
    return;
  }

  const uint32_t currentPage = std::min(readUint32(progressData), totalPages - 1);
  book.bookProgress = calculatePercentage(static_cast<uint64_t>(currentPage) + 1, totalPages);
  book.hasBookProgress = true;
}
}  // namespace

void RecentBookProgress::load(RecentBook& book) {
  book.hasBookProgress = false;
  book.bookProgress = 0;
  book.hasChapterProgress = false;
  book.chapterProgress = 0;
  book.currentChapter.clear();

  if (FsHelpers::hasEpubExtension(book.path)) {
    loadEpubProgress(book);
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    loadXtcProgress(book);
  } else if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    loadTxtProgress(book);
  }
}
