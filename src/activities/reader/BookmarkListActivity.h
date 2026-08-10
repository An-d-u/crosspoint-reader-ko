#pragma once

#include <string>

#include "BookmarkStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BookmarkListActivity final : public Activity {
 public:
  BookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                       Bookmark currentBookmark);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  static constexpr unsigned long deleteHoldMs = 700;

  BookmarkStore store;
  Bookmark currentBookmark;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool storageError = false;

  bool canToggleCurrent() const { return currentBookmark.pageCount > 0; }
  int getActionOffset() const { return canToggleCurrent() ? 1 : 0; }
  int getTotalItems() const;
  int getPageItems() const;
  int getBookmarkIndex() const;
  void toggleCurrentBookmark();
  void confirmDeleteBookmark(int bookmarkIndex);
};
