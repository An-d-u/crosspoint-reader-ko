#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>

#include "BookDataStore.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "RecentBooksStore.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

namespace {
void migrateBookmarks(const std::string& path) {
  BookmarkStore store(path);
  store.load();
}
}  // namespace

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  // 같은 독서 화면을 사용하되 MD는 별도의 서식 변환을 거친다.
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path) || FsHelpers::hasPdfExtension(path);
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path, const std::string& cacheKey) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint", cacheKey));
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path, const std::string& cacheKey) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint", cacheKey));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path, const std::string& cacheKey) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint", cacheKey));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    const BookDataReference bookData = BookDataStore::resolve(initialBookPath);
    auto xtc = loadXtc(initialBookPath, bookData.cacheKey);
    if (!xtc) {
      onGoBack();
      return;
    }
    migrateBookmarks(initialBookPath);
    RECENT_BOOKS.relocateBook(bookData.previousPath, initialBookPath);
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    const BookDataReference bookData = BookDataStore::resolve(initialBookPath);
    auto txt = loadTxt(initialBookPath, bookData.cacheKey);
    if (!txt) {
      onGoBack();
      return;
    }
    migrateBookmarks(initialBookPath);
    RECENT_BOOKS.relocateBook(bookData.previousPath, initialBookPath);
    onGoToTxtReader(std::move(txt));
  } else {
    const BookDataReference bookData = BookDataStore::resolve(initialBookPath);
    auto epub = loadEpub(initialBookPath, bookData.cacheKey);
    if (!epub) {
      onGoBack();
      return;
    }
    migrateBookmarks(initialBookPath);
    RECENT_BOOKS.relocateBook(bookData.previousPath, initialBookPath);
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
