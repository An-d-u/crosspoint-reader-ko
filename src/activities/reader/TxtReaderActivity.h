#pragma once

#include <Txt.h>
#include <Epub/Page.h>

#include <array>
#include <atomic>
#include <vector>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // EPUB과 같은 페이지 데이터를 SD에 저장하고 인접 페이지만 메모리에 유지한다.
  std::vector<uint32_t> pageOffsets;
  FsFile pageFile;
  int viewportWidth = 0;
  int viewportHeight = 0;
  bool initialized = false;
  bool layoutFailed = false;
  bool markdown = false;

  struct CachedTextPage {
    int pageNumber = -1;
    uint32_t lastUsed = 0;
    std::unique_ptr<Page> page;
  };
  static constexpr int PAGE_CACHE_SIZE = 3;
  static constexpr uint32_t MIN_PAGE_PREFETCH_HEAP = 64 * 1024;
  CachedTextPage pageCache[PAGE_CACHE_SIZE];
  uint32_t pageCacheClock = 0;

  std::atomic<bool> progressDirty{false};
  int pendingProgressPage = 0;
  unsigned long progressQueuedAt = 0;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  uint8_t cachedCharacterWrap = 0;
  float cachedLineCompression = 1.0f;
  bool cachedExtraParagraphSpacing = false;
  bool cachedParagraphIndent = false;
  bool cachedHyphenation = false;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(const Page& page);
  void renderStatusBar() const;

  void initializeReader();
  bool buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache();
  std::array<uint32_t, 16> indexHeader() const;
  bool saveProgress(int page) const;
  void queueProgressSave(int page);
  void flushPendingProgress();
  void loadProgress();
  void clearPageCache();
  const Page* loadCachedPage(int pageNumber, bool markUsed);
  void prefetchAdjacentPages();
  Bookmark getCurrentBookmark() const;
  void openReaderMenu();
  void openBookmarks();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput), txt(std::move(txt)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
};
