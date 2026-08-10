#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

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

uint8_t calculatePercentage(uint64_t current, uint64_t total) {
  if (total == 0) {
    return 0;
  }
  return static_cast<uint8_t>(std::min<uint64_t>(100, current * 100 / total));
}

bool readProgressFile(const std::string& path, uint8_t* data, size_t capacity, int& bytesRead) {
  FsFile file;
  if (!Storage.openFileForRead("HRS", path, file)) {
    return false;
  }
  bytesRead = file.read(data, capacity);
  file.close();
  return true;
}

void loadEpubProgress(RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  // 홈에서는 이미 만들어진 메타데이터 캐시만 읽고 EPUB을 다시 인덱싱하지 않습니다.
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

void loadReadingProgress(RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    loadEpubProgress(book);
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    loadXtcProgress(book);
  } else if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    loadTxtProgress(book);
  }
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 3;  // File Browser, Recents, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
#if CROSSPOINT_ENABLE_WEB_TRANSFER
  count += 1;  // File transfer
#endif
#if CROSSPOINT_ENABLE_OPDS
  if (hasOpdsUrl) {
    count++;
  }
#endif
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
    loadReadingProgress(recentBooks.back());
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // Recovery for devices whose recent_books.json was written by an earlier
    // firmware that cleared coverBmpPath on XTC thumb-gen OOM (before the
    // sleep-validation fix stopped deleting 1-bit thumbs on every sleep).
    // Rebuild the cache-path template from the book's extension so the
    // normal generation path below gets another chance.
    if (book.coverBmpPath.empty() && !book.path.empty()) {
      const auto h = std::to_string(std::hash<std::string>{}(book.path));
      if (FsHelpers::hasXtcExtension(book.path)) {
        book.coverBmpPath = "/.crosspoint/xtc_" + h + "/thumb_[HEIGHT].bmp";
      } else if (FsHelpers::hasEpubExtension(book.path)) {
        book.coverBmpPath = "/.crosspoint/epub_" + h + "/thumb_[HEIGHT].bmp";
      }
    }

    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          epub.generateThumbBmp(coverHeight);
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            xtc.generateThumbBmp(coverHeight);
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

#if CROSSPOINT_ENABLE_OPDS
  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;
#else
  hasOpdsUrl = false;
#endif

  selectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
#if CROSSPOINT_ENABLE_OPDS
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
#endif
#if CROSSPOINT_ENABLE_WEB_TRANSFER
    const int fileTransferIdx = idx++;
#endif
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
#if CROSSPOINT_ENABLE_OPDS
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
#endif
#if CROSSPOINT_ENABLE_WEB_TRANSFER
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
#endif
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Settings};

#if CROSSPOINT_ENABLE_WEB_TRANSFER
  menuItems.insert(menuItems.begin() + 2, tr(STR_FILE_TRANSFER));
  menuIcons.insert(menuIcons.begin() + 2, Transfer);
#endif
#if CROSSPOINT_ENABLE_OPDS
  if (hasOpdsUrl) {
    // Insert OPDS Browser after File Browser
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }
#endif

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
