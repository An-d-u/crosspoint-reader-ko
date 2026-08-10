#include "BookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int rowHeight = 52;
constexpr int listStartY = 75;
constexpr int listBottomMargin = 45;
}  // namespace

BookmarkListActivity::BookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string bookPath, Bookmark currentBookmark)
    : Activity("BookmarkList", renderer, mappedInput),
      store(std::move(bookPath)),
      currentBookmark(std::move(currentBookmark)) {}

void BookmarkListActivity::onEnter() {
  Activity::onEnter();
  storageError = !store.load();
  selectorIndex = 0;
  requestUpdate();
}

int BookmarkListActivity::getTotalItems() const {
  return getActionOffset() + static_cast<int>(store.getBookmarks().size());
}

int BookmarkListActivity::getPageItems() const {
  const auto orientation = renderer.getOrientation();
  const int contentY = orientation == GfxRenderer::Orientation::PortraitInverted ? 50 : 0;
  const int availableHeight = renderer.getScreenHeight() - contentY - listStartY - listBottomMargin;
  return std::max(1, availableHeight / rowHeight);
}

int BookmarkListActivity::getBookmarkIndex() const { return selectorIndex - getActionOffset(); }

void BookmarkListActivity::toggleCurrentBookmark() {
  const auto result = store.toggle(currentBookmark);
  storageError = result == BookmarkStore::ToggleResult::Failed;
  requestUpdate();
}

void BookmarkListActivity::confirmDeleteBookmark(const int bookmarkIndex) {
  if (bookmarkIndex < 0 || bookmarkIndex >= static_cast<int>(store.getBookmarks().size())) {
    return;
  }

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOKMARK),
                                             tr(STR_DELETE_BOOKMARK_CONFIRM)),
      [this, bookmarkIndex](const ActivityResult& result) {
        if (!result.isCancelled) {
          storageError = !store.removeAt(bookmarkIndex);
          const int totalItems = getTotalItems();
          selectorIndex = totalItems > 0 ? std::min(selectorIndex, totalItems - 1) : 0;
        }
      });
}

void BookmarkListActivity::loop() {
  const int totalItems = getTotalItems();
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (canToggleCurrent() && selectorIndex == 0) {
      toggleCurrentBookmark();
      return;
    }

    const int bookmarkIndex = getBookmarkIndex();
    if (mappedInput.getHeldTime() >= deleteHoldMs) {
      confirmDeleteBookmark(bookmarkIndex);
      return;
    }

    if (bookmarkIndex >= 0 && bookmarkIndex < static_cast<int>(store.getBookmarks().size())) {
      const Bookmark& bookmark = store.getBookmarks()[bookmarkIndex];
      setResult(BookmarkResult{bookmark.spineIndex, bookmark.page, bookmark.pageCount});
      finish();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (totalItems == 0) {
    return;
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void BookmarkListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = screenWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;

  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, contentX + (contentWidth - titleWidth) / 2, 12 + contentY, tr(STR_BOOKMARKS), true,
                    EpdFontFamily::BOLD);

  if (storageError) {
    renderer.drawCenteredText(UI_10_FONT_ID, 40 + contentY, tr(STR_BOOKMARK_STORAGE_ERROR));
  } else if (!store.getBookmarks().empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 40 + contentY, tr(STR_HOLD_CONFIRM_DELETE));
  }

  const int totalItems = getTotalItems();
  const int pageItems = getPageItems();
  const int pageStartIndex = totalItems > 0 ? selectorIndex / pageItems * pageItems : 0;
  const int actionOffset = getActionOffset();

  for (int row = 0; row < pageItems; ++row) {
    const int itemIndex = pageStartIndex + row;
    if (itemIndex >= totalItems) {
      break;
    }

    const int displayY = listStartY + contentY + row * rowHeight;
    const bool selected = itemIndex == selectorIndex;
    if (selected) {
      renderer.fillRect(contentX, displayY - 2, contentWidth - 1, rowHeight, true);
    }

    std::string title;
    std::string subtitle;
    if (actionOffset == 1 && itemIndex == 0) {
      title = store.findIndex(currentBookmark) >= 0 ? tr(STR_REMOVE_BOOKMARK) : tr(STR_ADD_BOOKMARK);
      title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
      char position[96];
      snprintf(position, sizeof(position), tr(STR_BOOKMARK_POSITION_FORMAT), currentBookmark.bookProgress,
               static_cast<unsigned long>(currentBookmark.page + 1));
      subtitle = position;
    } else {
      const Bookmark& bookmark = store.getBookmarks()[itemIndex - actionOffset];
      title = bookmark.chapter.empty() ? tr(STR_BOOKMARK) : bookmark.chapter;
      title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
      char position[96];
      snprintf(position, sizeof(position), tr(STR_BOOKMARK_POSITION_FORMAT), bookmark.bookProgress,
               static_cast<unsigned long>(bookmark.page + 1));
      subtitle = position;
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, title.c_str(), !selected, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY + 25, subtitle.c_str(), !selected);
  }

  if (store.getBookmarks().empty() && !storageError) {
    const int emptyY = std::min(screenHeight - listBottomMargin - 30, listStartY + contentY + rowHeight + 25);
    renderer.drawCenteredText(UI_10_FONT_ID, emptyY, tr(STR_NO_BOOKMARKS));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
