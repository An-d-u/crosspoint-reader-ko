#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <esp_system.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "BookmarkListActivity.h"
#include "DocumentReaderMenuActivity.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
constexpr unsigned long PROGRESS_SAVE_DELAY_MS = 750;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 4;          // Increment when cache format changes (added lineCompression)

// Find UTF-8 character boundary at or before pos
size_t findUtf8Boundary(const std::string& str, size_t pos) {
  if (pos >= str.length()) return str.length();
  // Move back if we're in the middle of a UTF-8 sequence
  while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
    pos--;
  }
  return pos;
}

// Binary search to find max characters that fit in width
// Returns the position (byte offset) where to break the string
size_t findBreakPosition(const GfxRenderer& renderer, int fontId, const std::string& line, int maxWidth) {
  if (line.empty()) return 0;

  // First check if the whole line fits
  int fullWidth = renderer.getTextWidth(fontId, line.c_str());
  if (fullWidth <= maxWidth) {
    return line.length();
  }

  // Binary search for the break point
  size_t low = 1;  // At minimum 1 character
  size_t high = line.length();
  size_t bestFit = 1;

  while (low < high) {
    size_t mid = (low + high + 1) / 2;
    mid = findUtf8Boundary(line, mid);

    if (mid <= low) {
      // Can't make progress, exit
      break;
    }

    std::string substr = line.substr(0, mid);
    int width = renderer.getTextWidth(fontId, substr.c_str());

    if (width <= maxWidth) {
      bestFit = mid;
      low = mid;
    } else {
      high = mid - 1;
      if (high > 0) {
        high = findUtf8Boundary(line, high);
      }
    }
  }

  // Try to break at word boundary (space) if possible, unless character wrap is enabled
  if (!SETTINGS.characterWrap && bestFit > 0 && bestFit < line.length()) {
    size_t spacePos = line.rfind(' ', bestFit);
    if (spacePos != std::string::npos && spacePos > 0) {
      // Check if breaking at space still fits
      std::string atSpace = line.substr(0, spacePos);
      if (renderer.getTextWidth(fontId, atSpace.c_str()) <= maxWidth) {
        return spacePos;
      }
    }
  }

  return bestFit > 0 ? bestFit : 1;  // At minimum, consume 1 character
}
}  // namespace

void TxtReaderActivity::clearPageCache() {
  for (auto& cached : pageCache) {
    cached.pageNumber = -1;
    cached.lastUsed = 0;
    cached.lines.clear();
  }
  pageCacheClock = 0;
}

const std::vector<std::string>* TxtReaderActivity::loadCachedPage(const int pageNumber, const bool markUsed) {
  if (pageNumber < 0 || pageNumber >= totalPages || static_cast<size_t>(pageNumber) >= pageOffsets.size()) {
    return nullptr;
  }

  for (auto& cached : pageCache) {
    if (cached.pageNumber == pageNumber && !cached.lines.empty()) {
      if (markUsed) cached.lastUsed = ++pageCacheClock;
      return &cached.lines;
    }
  }

  std::vector<std::string> lines;
  size_t nextOffset = pageOffsets[pageNumber];
  if (!loadPageAtOffset(pageOffsets[pageNumber], lines, nextOffset)) {
    return nullptr;
  }

  CachedTextPage* target = &pageCache[0];
  for (auto& cached : pageCache) {
    if (cached.pageNumber < 0) {
      target = &cached;
      break;
    }
    if (cached.lastUsed < target->lastUsed) {
      target = &cached;
    }
  }

  target->pageNumber = pageNumber;
  target->lastUsed = markUsed ? ++pageCacheClock : 0;
  target->lines = std::move(lines);
  return &target->lines;
}

void TxtReaderActivity::prefetchAdjacentPages() {
  const int adjacentPages[] = {currentPage - 1, currentPage + 1};
  for (const int pageNumber : adjacentPages) {
    if (pageNumber < 0 || pageNumber >= totalPages) continue;
    if (esp_get_free_heap_size() < MIN_PAGE_PREFETCH_HEAP) {
      LOG_DBG("TRS", "Skipping page prefetch at %lu bytes free heap", esp_get_free_heap_size());
      return;
    }
    loadCachedPage(pageNumber, false);
  }
}

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  flushPendingProgress();
  clearPageCache();
  pageOffsets.clear();
  readBuffer.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

Bookmark TxtReaderActivity::getCurrentBookmark() const {
  Bookmark bookmark;
  if (!initialized || totalPages <= 0 || currentPage < 0 || currentPage >= totalPages) {
    return bookmark;
  }

  bookmark.page = currentPage;
  bookmark.pageCount = totalPages;
  bookmark.bookProgress = static_cast<uint8_t>(std::min(100, (currentPage + 1) * 100 / totalPages));
  return bookmark;
}

void TxtReaderActivity::openBookmarks() {
  if (!txt || !initialized) {
    return;
  }

  startActivityForResult(
      std::make_unique<BookmarkListActivity>(renderer, mappedInput, txt->getPath(), getCurrentBookmark()),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && totalPages > 0) {
          const uint32_t page = std::get<BookmarkResult>(result.data).page;
          currentPage = static_cast<int>(std::min<uint32_t>(page, static_cast<uint32_t>(totalPages - 1)));
        }
      });
}

void TxtReaderActivity::openReaderMenu() {
  if (!txt || !initialized || totalPages <= 0) {
    return;
  }

  const int progress = std::min(100, (currentPage + 1) * 100 / totalPages);
  startActivityForResult(
      std::make_unique<DocumentReaderMenuActivity>(renderer, mappedInput, txt->getTitle(), currentPage + 1,
                                                   totalPages, progress, false),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          return;
        }

        const auto action =
            static_cast<DocumentReaderMenuActivity::MenuAction>(std::get<MenuResult>(result.data).action);
        switch (action) {
          case DocumentReaderMenuActivity::MenuAction::Bookmarks:
            openBookmarks();
            break;
          case DocumentReaderMenuActivity::MenuAction::GoHome:
            onGoHome();
            break;
          case DocumentReaderMenuActivity::MenuAction::SelectChapter:
            break;
        }
      });
}

void TxtReaderActivity::loop() {
  if (progressDirty.load(std::memory_order_acquire) && millis() - progressQueuedAt >= PROGRESS_SAVE_DELAY_MS &&
      !RenderLock::peek()) {
    RenderLock lock(*this);
    if (progressDirty.load(std::memory_order_relaxed)) {
      flushPendingProgress();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openReaderMenu();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedCharacterWrap = SETTINGS.characterWrap;
  cachedLineCompression = SETTINGS.getReaderLineCompression();

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  readBuffer.resize(chunkSize + 1);
  uint8_t* buffer = readBuffer.data();

  if (!txt->readContent(buffer, offset, chunkSize)) {
    return false;
  }
  buffer[chunkSize] = '\0';

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Word wrap if needed - use binary search for performance with SD fonts
    while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage) {
      // Use binary search to find break position (much faster than linear search)
      size_t breakPos = findBreakPosition(renderer, cachedFontId, line, viewportWidth);

      if (breakPos >= line.length()) {
        // Whole line fits
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }

      if (breakPos == 0) {
        breakPos = 1;  // Ensure progress
      }

      outLines.push_back(line.substr(0, breakPos));

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    }

    // Determine how much of the source buffer we consumed
    if (line.empty()) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Check if font or settings changed since initialization
  if (initialized) {
    const int currentFontId = SETTINGS.getReaderFontId();
    const int currentMargin = SETTINGS.screenMargin;
    const uint8_t currentAlignment = SETTINGS.paragraphAlignment;
    const uint8_t currentCharacterWrap = SETTINGS.characterWrap;
    const float currentLineCompression = SETTINGS.getReaderLineCompression();

    if (currentFontId != cachedFontId || currentMargin != cachedScreenMargin ||
        currentAlignment != cachedParagraphAlignment || currentCharacterWrap != cachedCharacterWrap ||
        currentLineCompression != cachedLineCompression) {
      LOG_DBG("TRS", "Settings changed, reinitializing (font: %d->%d)", cachedFontId, currentFontId);
      initialized = false;
      clearPageCache();
      pageOffsets.clear();
    }
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  LOG_DBG("TRS", "Loading page %d content...", currentPage);

  const std::vector<std::string>* pageLines = loadCachedPage(currentPage, true);
  if (!pageLines) {
    LOG_ERR("TRS", "Failed to load page %d", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  LOG_DBG("TRS", "Page loaded, %d lines. Rendering...", pageLines->size());

  renderer.clearScreen();
  renderPage(*pageLines);
  prefetchAdjacentPages();

  queueProgressSave(currentPage);
}

void TxtReaderActivity::renderPage(const std::vector<std::string>& lines) {
  const int lineHeight = renderer.getLineHeight(cachedFontId) * cachedLineCompression;
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : lines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;

        // Apply text alignment
        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            int textWidth = renderer.getTextWidth(cachedFontId, line.c_str());
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

bool TxtReaderActivity::saveProgress(const int page) const {
  if (!txt) return false;

  FsFile f;
  if (Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = page & 0xFF;
    data[1] = (page >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    if (f.write(data, 4) == 4) return true;
    LOG_ERR("TRS", "Failed to write progress for page %d", page);
  } else {
    LOG_ERR("TRS", "Failed to open progress file for page %d", page);
  }
  return false;
}

void TxtReaderActivity::queueProgressSave(const int page) {
  pendingProgressPage = page;
  progressQueuedAt = millis();
  progressDirty.store(true, std::memory_order_release);
}

void TxtReaderActivity::flushPendingProgress() {
  if (!progressDirty.load(std::memory_order_relaxed)) return;
  if (saveProgress(pendingProgressPage)) {
    progressDirty.store(false, std::memory_order_release);
  }
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("TRS", "Loaded progress: page %d/%d", currentPage, totalPages);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t characterWrap;
  serialization::readPod(f, characterWrap);
  if (characterWrap != cachedCharacterWrap) {
    LOG_DBG("TRS", "Cache character wrap mismatch, rebuilding");
    f.close();
    return false;
  }

  float lineCompression;
  serialization::readPod(f, lineCompression);
  if (lineCompression != cachedLineCompression) {
    LOG_DBG("TRS", "Cache line compression mismatch, rebuilding");
    f.close();
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedCharacterWrap);
  serialization::writePod(f, cachedLineCompression);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}
