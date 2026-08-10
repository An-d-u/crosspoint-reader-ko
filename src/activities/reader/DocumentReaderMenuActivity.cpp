#include "DocumentReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

std::vector<DocumentReaderMenuActivity::MenuItem> DocumentReaderMenuActivity::buildMenuItems(const bool hasChapters) {
  std::vector<MenuItem> items;
  items.reserve(3);
  if (hasChapters) {
    items.push_back({MenuAction::SelectChapter, StrId::STR_SELECT_CHAPTER});
  }
  items.push_back({MenuAction::Bookmarks, StrId::STR_BOOKMARKS});
  items.push_back({MenuAction::GoHome, StrId::STR_GO_HOME_BUTTON});
  return items;
}

DocumentReaderMenuActivity::DocumentReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::string title, const int currentPage, const int totalPages,
                                                       const int bookProgress, const bool hasChapters)
    : Activity("DocumentReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasChapters)),
      title(std::move(title)),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgress(bookProgress) {}

void DocumentReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void DocumentReaderMenuActivity::loop() {
  const int itemCount = static_cast<int>(menuItems.size());
  buttonNavigator.onNext([this, itemCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, itemCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(MenuResult{static_cast<int>(menuItems[selectorIndex].action), 0, 0});
    finish();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void DocumentReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = screenWidth - hintGutterWidth;
  const int contentY = isPortraitInverted ? 50 : 0;

  const std::string safeTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, safeTitle.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, contentX + (contentWidth - titleWidth) / 2, 15 + contentY, safeTitle.c_str(), true,
                    EpdFontFamily::BOLD);

  char progress[96];
  snprintf(progress, sizeof(progress), tr(STR_READER_PROGRESS_FORMAT), currentPage, totalPages, bookProgress);
  renderer.drawCenteredText(UI_10_FONT_ID, 45 + contentY, progress);

  constexpr int startY = 80;
  constexpr int menuRowHeight = 40;
  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY = startY + contentY + static_cast<int>(i) * menuRowHeight;
    const bool selected = static_cast<int>(i) == selectorIndex;
    if (selected) {
      renderer.fillRect(contentX, displayY - 2, contentWidth - 1, menuRowHeight, true);
    }
    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, I18N.get(menuItems[i].labelId), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
