#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DocumentReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction { SelectChapter, Bookmarks, GoHome };

  DocumentReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                             int currentPage, int totalPages, int bookProgress, bool hasChapters);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasChapters);

  std::vector<MenuItem> menuItems;
  ButtonNavigator buttonNavigator;
  std::string title;
  int selectorIndex = 0;
  int currentPage = 0;
  int totalPages = 0;
  int bookProgress = 0;
};
