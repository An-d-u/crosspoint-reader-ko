#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <esp_heap_caps.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "TextDocumentMarkup.h"
#include "TextReaderCache.h"

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
constexpr size_t CHUNK_SIZE = 1024;
constexpr unsigned long PROGRESS_SAVE_DELAY_MS = 750;
constexpr size_t MAX_PAGES = 65535;

// 벡터 교체 할당에는 증가분이 아닌 새 버퍼 전체 크기의 연속 메모리가 필요하다.
bool reservePageOffset(std::vector<uint32_t>& offsets) {
  if (offsets.size() >= MAX_PAGES) return false;
  if (offsets.size() < offsets.capacity()) return true;
  const size_t capacity = std::min(MAX_PAGES, offsets.size() + 128);
  const size_t bytes = capacity * sizeof(uint32_t);
  if (esp_get_free_heap_size() < bytes + 8192 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < bytes + 256) return false;
  offsets.reserve(capacity);
  return true;
}
}  // namespace

void TxtReaderActivity::clearPageCache() {
  for (auto& cached : pageCache) {
    cached.pageNumber = -1;
    cached.lastUsed = 0;
    cached.page.reset();
  }
  pageCacheClock = 0;
}

const Page* TxtReaderActivity::loadCachedPage(const int pageNumber, const bool markUsed) {
  if (pageNumber < 0 || static_cast<size_t>(pageNumber) >= pageOffsets.size()) return nullptr;
  for (auto& cached : pageCache) {
    if (cached.pageNumber == pageNumber && cached.page) {
      if (markUsed) cached.lastUsed = ++pageCacheClock;
      return cached.page.get();
    }
  }
  CachedTextPage* target = &pageCache[0];
  for (auto& cached : pageCache) {
    if (!cached.page) { target = &cached; break; }
    if (cached.lastUsed < target->lastUsed) target = &cached;
  }
  target->page.reset();
  target->pageNumber = -1;
  if (!pageFile.isOpen() &&
      !Storage.openFileForRead("TRS", txt->getCachePath() + "/text-pages.bin", pageFile)) return nullptr;
  if (!pageFile.seek(pageOffsets[pageNumber])) return nullptr;
  target->page = Page::deserialize(pageFile);
  if (!target->page) return nullptr;
  target->pageNumber = pageNumber;
  target->lastUsed = markUsed ? ++pageCacheClock : 0;
  return target->page.get();
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
  if (pageFile.isOpen()) pageFile.close();
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
  if (initialized) return;
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedCharacterWrap = SETTINGS.characterWrap;
  cachedLineCompression = SETTINGS.getReaderLineCompression();
  cachedExtraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  cachedParagraphIndent = SETTINGS.paragraphIndent;
  cachedHyphenation = SETTINGS.hyphenationEnabled;
  markdown = FsHelpers::hasMarkdownExtension(txt->getPath());

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));
  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;

  layoutFailed = false;
  if (!loadPageIndexCache()) {
    if (!buildPageIndex()) layoutFailed = true;
    else savePageIndexCache();
  }
  totalPages = pageOffsets.size();
  loadProgress();
  initialized = true;
}

bool TxtReaderActivity::buildPageIndex() {
  if (pageFile.isOpen()) pageFile.close();
  pageOffsets.clear();
  const std::string markupPath = txt->getCachePath() + "/text-source.tmp";
  const std::string pagesPath = txt->getCachePath() + "/text-pages.bin";
  // 새 캐시가 완성되기 전에는 이전 색인이 새 데이터와 조합되지 않아야 한다.
  Storage.remove((txt->getCachePath() + "/index.bin").c_str());
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  bool converted = false;
  {
    FsFile source, markup;
    if (!Storage.openFileForRead("TRS", txt->getPath(), source) ||
        !Storage.openFileForWrite("TRS", markupPath, markup)) return false;
    // 읽기/쓰기 버퍼를 변환 중에만 유지하고 EPUB 페이지 구성 전에 해제한다.
    std::vector<char> input(CHUNK_SIZE), output;
    output.reserve(CHUNK_SIZE);
    auto flush = [&]() {
      const bool ok = output.empty() || markup.write(output.data(), output.size()) == output.size();
      output.clear();
      return ok;
    };
    auto sink = [&](std::string_view value) {
      for (char ch : value) {
        output.push_back(ch);
        if (output.size() == CHUNK_SIZE && !flush()) return false;
      }
      return true;
    };
    TextDocumentMarkup::Converter converter(sink, markdown);
    converted = converter.begin();
    while (converted && source.available()) {
      const int count = source.read(input.data(), input.size());
      if (count <= 0) { converted = false; break; }
      converted = converter.push(std::string_view(input.data(), count));
      vTaskDelay(1);
    }
    converted = converted && converter.finish() && flush();
    converted = markup.close() && converted;
  }
  if (!converted) {
    Storage.remove(markupPath.c_str());
    return false;
  }

  bool written = true;
  bool parsed = false;
  {
    FsFile pages;
    if (!Storage.openFileForWrite("TRS", pagesPath, pages)) {
      Storage.remove(markupPath.c_str());
      return false;
    }
    Hyphenator::setPreferredLanguage("");
    // 외부 CSS를 읽지 않고 변환기가 생성한 목록·코드의 인라인 스타일만 해석한다.
    CssParser inlineStyles("");
    ChapterHtmlSlimParser parser(
        nullptr, markupPath, renderer, cachedFontId, UI_FONT_ID, cachedLineCompression,
        cachedExtraParagraphSpacing, cachedParagraphIndent, cachedParagraphAlignment, cachedCharacterWrap,
        viewportWidth, viewportHeight, cachedHyphenation,
        [&](std::unique_ptr<Page> page) {
          if (!written || !page || page->elements.empty()) return;
          if (!reservePageOffset(pageOffsets)) { written = false; return; }
          pageOffsets.push_back(pages.position());
          if (!page->serialize(pages) || pages.hasWriteError()) written = false;
          vTaskDelay(1);
        },
        true, "", "", 0, false, nullptr, &inlineStyles);
    parsed = parser.parseAndBuildPages();
    written = pages.close() && written;
  }
  Storage.remove(markupPath.c_str());
  if (!parsed || !written) {
    pageOffsets.clear();
    Storage.remove(pagesPath.c_str());
    return false;
  }
  return Storage.openFileForRead("TRS", pagesPath, pageFile);
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
        currentLineCompression != cachedLineCompression ||
        SETTINGS.extraParagraphSpacing != cachedExtraParagraphSpacing ||
        SETTINGS.paragraphIndent != cachedParagraphIndent || SETTINGS.hyphenationEnabled != cachedHyphenation) {
      LOG_DBG("TRS", "Settings changed, reinitializing (font: %d->%d)", cachedFontId, currentFontId);
      initialized = false;
      clearPageCache();
      pageOffsets.clear();
      if (pageFile.isOpen()) pageFile.close();
    }
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (layoutFailed || pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, layoutFailed ? tr(STR_PAGE_LOAD_ERROR) : tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  LOG_DBG("TRS", "Loading page %d content...", currentPage);

  const Page* pageLines = loadCachedPage(currentPage, true);
  if (!pageLines) {
    LOG_ERR("TRS", "Failed to load page %d", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  LOG_DBG("TRS", "Page loaded, %d lines. Rendering...", pageLines->elements.size());

  renderer.clearScreen();
  renderPage(*pageLines);
  prefetchAdjacentPages();

  queueProgressSave(currentPage);
}

void TxtReaderActivity::renderPage(const Page& page) {
  auto renderLines = [&]() {
    page.render(renderer, cachedFontId, UI_FONT_ID, cachedOrientedMarginLeft, cachedOrientedMarginTop);
  };
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();
  scope.endScanAndPrewarm();
  renderLines();
  renderStatusBar();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
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

std::array<uint32_t, 16> TxtReaderActivity::indexHeader() const {
  uint32_t compression = 0;
  static_assert(sizeof(compression) == sizeof(cachedLineCompression));
  memcpy(&compression, &cachedLineCompression, sizeof(compression));
  return {TextReaderCache::MAGIC, TextReaderCache::VERSION, static_cast<uint32_t>(txt->getFileSize()),
          static_cast<uint32_t>(viewportWidth), static_cast<uint32_t>(viewportHeight),
          static_cast<uint32_t>(cachedFontId), static_cast<uint32_t>(UI_FONT_ID),
          cachedParagraphAlignment, cachedCharacterWrap, compression,
          cachedExtraParagraphSpacing, cachedParagraphIndent, cachedHyphenation, markdown, 0, 0};
}

bool TxtReaderActivity::loadPageIndexCache() {
  FsFile index;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/index.bin", index) ||
      !Storage.openFileForRead("TRS", txt->getCachePath() + "/text-pages.bin", pageFile)) return false;
  std::array<uint32_t, 16> header{};
  const auto expected = indexHeader();
  if (index.read(header.data(), sizeof(header)) != sizeof(header) ||
      !std::equal(header.begin(), header.begin() + 14, expected.begin()) ||
      header[14] != pageFile.size() || header[15] > MAX_PAGES ||
      index.size() != sizeof(header) + header[15] * sizeof(uint32_t)) return false;
  pageOffsets.clear();
  for (uint32_t i = 0; i < header[15]; ++i) {
    uint32_t offset = 0;
    if (index.read(&offset, sizeof(offset)) != sizeof(offset) || offset >= pageFile.size() ||
        (i == 0 && offset != 0) || (i > 0 && offset <= pageOffsets.back()) || !reservePageOffset(pageOffsets)) {
      pageOffsets.clear();
      return false;
    }
    pageOffsets.push_back(offset);
  }
  return true;
}

void TxtReaderActivity::savePageIndexCache() {
  FsFile index;
  const std::string path = txt->getCachePath() + "/index.bin";
  if (!Storage.openFileForWrite("TRS", path, index)) return;
  auto header = indexHeader();
  header[14] = pageFile.size();
  header[15] = pageOffsets.size();
  bool ok = index.write(header.data(), sizeof(header)) == sizeof(header);
  for (uint32_t offset : pageOffsets) {
    if (!ok) break;
    ok = index.write(&offset, sizeof(offset)) == sizeof(offset);
  }
  ok = index.close() && ok;
  if (!ok) Storage.remove(path.c_str());
}
