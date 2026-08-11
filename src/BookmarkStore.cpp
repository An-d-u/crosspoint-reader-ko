#include "BookmarkStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "BookDataStore.h"

namespace {
constexpr int bookmarkFileVersion = 2;
constexpr int legacyBookmarkFileVersion = 1;
constexpr char bookmarkDirectory[] = "/.crosspoint/bookmarks";
constexpr size_t maxChapterLength = 256;

uint64_t stablePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char character : path) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string legacyBookmarkPath(const std::string& path) {
  return std::string(bookmarkDirectory) + "/book_" + std::to_string(stablePathHash(path)) + ".json";
}
}  // namespace

BookmarkStore::BookmarkStore(std::string bookPath) : bookPath(std::move(bookPath)) {
  const BookDataReference bookData = BookDataStore::resolve(this->bookPath);
  bookId = bookData.id;
  knownPaths = bookData.knownPaths;
  storagePath = bookId.empty() ? legacyBookmarkPath(this->bookPath)
                               : std::string(bookmarkDirectory) + "/book_id_" + bookId + ".json";
}

bool BookmarkStore::load() {
  bookmarks.clear();
  if (Storage.exists(storagePath.c_str())) {
    return loadFromFile(storagePath);
  }

  if (bookId.empty()) {
    return true;
  }

  if (std::find(knownPaths.begin(), knownPaths.end(), bookPath) == knownPaths.end()) {
    knownPaths.push_back(bookPath);
  }
  for (const std::string& path : knownPaths) {
    const std::string legacyPath = legacyBookmarkPath(path);
    if (!Storage.exists(legacyPath.c_str())) {
      continue;
    }
    if (!loadFromFile(legacyPath, path)) {
      continue;
    }
    // 기존 파일은 롤백을 위해 남겨두고 내용 지문 기반 파일을 새로 저장합니다.
    return save();
  }
  return true;
}

bool BookmarkStore::loadFromFile(const std::string& path, const std::string& legacyBookPath) {
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, json);
  if (error) {
    LOG_ERR("BMK", "JSON parse error: %s", error.c_str());
    return false;
  }

  const int version = document["version"] | 0;
  const bool validLegacy = !legacyBookPath.empty() && version == legacyBookmarkFileVersion &&
                           (document["bookPath"] | std::string()) == legacyBookPath;
  const bool validCurrent = legacyBookPath.empty() && version == bookmarkFileVersion && !bookId.empty() &&
                            (document["bookId"] | std::string()) == bookId;
  const bool validPathFallback = legacyBookPath.empty() && bookId.empty() && version == legacyBookmarkFileVersion &&
                                 (document["bookPath"] | std::string()) == bookPath;
  if (!validLegacy && !validCurrent && !validPathFallback) {
    LOG_ERR("BMK", "Bookmark file identity mismatch");
    return false;
  }

  const JsonArrayConst items = document["bookmarks"].as<JsonArrayConst>();
  std::vector<Bookmark> loadedBookmarks;
  loadedBookmarks.reserve(std::min(items.size(), maxBookmarks));
  for (const JsonObjectConst item : items) {
    if (loadedBookmarks.size() >= maxBookmarks) {
      break;
    }

    Bookmark bookmark;
    bookmark.spineIndex = item["spineIndex"] | -1;
    bookmark.page = item["page"] | 0U;
    bookmark.pageCount = item["pageCount"] | 0U;
    bookmark.bookProgress = std::min<uint8_t>(item["bookProgress"] | 0U, 100);
    bookmark.chapter = item["chapter"] | std::string();

    if (bookmark.pageCount == 0 || bookmark.page >= bookmark.pageCount) {
      continue;
    }
    if (bookmark.chapter.size() > maxChapterLength) {
      bookmark.chapter.resize(maxChapterLength);
    }
    loadedBookmarks.push_back(std::move(bookmark));
  }

  bookmarks = std::move(loadedBookmarks);
  return true;
}

bool BookmarkStore::save() const {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(bookmarkDirectory);

  JsonDocument document;
  document["version"] = bookId.empty() ? legacyBookmarkFileVersion : bookmarkFileVersion;
  document["bookPath"] = bookPath;
  if (!bookId.empty()) {
    document["bookId"] = bookId;
  }
  JsonArray items = document["bookmarks"].to<JsonArray>();

  for (const Bookmark& bookmark : bookmarks) {
    JsonObject item = items.add<JsonObject>();
    item["spineIndex"] = bookmark.spineIndex;
    item["page"] = bookmark.page;
    item["pageCount"] = bookmark.pageCount;
    item["bookProgress"] = bookmark.bookProgress;
    item["chapter"] = bookmark.chapter;
  }

  String json;
  serializeJson(document, json);
  return Storage.writeFile(storagePath.c_str(), json);
}

BookmarkStore::ToggleResult BookmarkStore::toggle(const Bookmark& bookmark) {
  if (bookmark.pageCount == 0 || bookmark.page >= bookmark.pageCount) {
    return ToggleResult::Failed;
  }

  const int existingIndex = findIndex(bookmark);
  if (existingIndex >= 0) {
    Bookmark removed = bookmarks[existingIndex];
    bookmarks.erase(bookmarks.begin() + existingIndex);
    if (save()) {
      return ToggleResult::Removed;
    }
    bookmarks.insert(bookmarks.begin() + existingIndex, std::move(removed));
    return ToggleResult::Failed;
  }

  if (bookmarks.size() >= maxBookmarks) {
    return ToggleResult::Failed;
  }

  Bookmark saved = bookmark;
  saved.bookProgress = std::min<uint8_t>(saved.bookProgress, 100);
  if (saved.chapter.size() > maxChapterLength) {
    saved.chapter.resize(maxChapterLength);
  }
  bookmarks.insert(bookmarks.begin(), std::move(saved));
  if (save()) {
    return ToggleResult::Added;
  }
  bookmarks.erase(bookmarks.begin());
  return ToggleResult::Failed;
}

bool BookmarkStore::removeAt(const size_t index) {
  if (index >= bookmarks.size()) {
    return false;
  }

  Bookmark removed = bookmarks[index];
  bookmarks.erase(bookmarks.begin() + index);
  if (save()) {
    return true;
  }
  bookmarks.insert(bookmarks.begin() + index, std::move(removed));
  return false;
}

int BookmarkStore::findIndex(const Bookmark& bookmark) const {
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    if (isSamePosition(bookmarks[i], bookmark)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool BookmarkStore::isSamePosition(const Bookmark& left, const Bookmark& right) {
  if (left.spineIndex != right.spineIndex) {
    return false;
  }

  // 페이지 기반 형식은 레이아웃이 변하지 않으므로 정확한 페이지 번호를 비교합니다.
  if (left.spineIndex < 0 || left.pageCount == right.pageCount) {
    return left.page == right.page;
  }

  // EPUB은 글꼴이나 여백 변경으로 페이지 수가 바뀔 수 있어 챕터 내 상대 위치를 비교합니다.
  const double leftProgress = static_cast<double>(left.page) / left.pageCount;
  const double rightProgress = static_cast<double>(right.page) / right.pageCount;
  const double tolerance = 0.5 / left.pageCount + 0.5 / right.pageCount;
  return std::abs(leftProgress - rightProgress) <= tolerance;
}
