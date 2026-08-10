#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Bookmark {
  int spineIndex = -1;
  uint32_t page = 0;
  uint32_t pageCount = 0;
  uint8_t bookProgress = 0;
  std::string chapter;
};

class BookmarkStore {
 public:
  enum class ToggleResult { Added, Removed, Failed };

  explicit BookmarkStore(std::string bookPath);

  bool load();
  ToggleResult toggle(const Bookmark& bookmark);
  bool removeAt(size_t index);
  int findIndex(const Bookmark& bookmark) const;

  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

 private:
  static constexpr size_t maxBookmarks = 50;

  std::string bookPath;
  std::string storagePath;
  std::vector<Bookmark> bookmarks;

  bool save() const;
  static bool isSamePosition(const Bookmark& left, const Bookmark& right);
};
