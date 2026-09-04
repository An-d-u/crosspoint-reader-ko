#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GeekNewsScrapStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class FeedLoadError : uint8_t { None, Wifi, Dns, Tls, Http, Response, StorageIo, Parse, Memory, LayoutLimit };

struct FeedLoadDiagnostics {
  enum class Stage : uint8_t { None, Headers, Body };
  enum class MemoryStage : uint8_t {
    None, ArticleBuffer, LineBuffer, SpanBuffer, TextBuffer, PageBuffer, LineLimit, SpanLimit, TextLimit
  };
  Stage stage = Stage::None;
  int code = 0;
  int expected = -1;
  size_t received = 0;
  uint32_t elapsedMs = 0;
  bool sinkFailed = false;
  bool timedOut = false;
  MemoryStage memoryStage = MemoryStage::None;
  size_t requestedBytes = 0;
  size_t freeHeap = 0;
  size_t largestBlock = 0;
};

class GeekNewsActivity final : public Activity {
 public:
  explicit GeekNewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Feed", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class View : uint8_t { Sources, LoadingTopics, Topics, LoadingArticle, Article, Scraps, ConfirmDelete, Error };
  enum class FeedSource : uint8_t { GeekNews, HackerNews };
  enum class InlineStyle : uint8_t { Regular, Bold, Italic, Code, Link };

  struct Topic {
    int id = 0;
    std::string title;
    std::string subtitle;
    std::string url;
    std::string text;
  };

  struct Span {
    std::string text;
    InlineStyle style = InlineStyle::Regular;
  };

  struct StoredSpan {
    uint16_t textOffset = 0;
    InlineStyle style = InlineStyle::Regular;
  };

  struct RichLine {
    uint16_t spanStart = 0;
    uint16_t spanCount = 0;
    int16_t indent = 0;
    int16_t height = 0;
    bool quote = false;
    bool rule = false;
  };

  struct FeedStreamContext {
    std::string pending;
    std::vector<Topic>* topics = nullptr;
  };

  static constexpr int kMaxTopics = 20;

  ButtonNavigator buttonNavigator_;
  GeekNewsScrapStore scrapStore_;
  View view_ = View::Sources;
  FeedSource source_ = FeedSource::GeekNews;
  std::vector<Topic> topics_;
  std::vector<RichLine> articleLines_;
  std::vector<StoredSpan> articleSpans_;
  std::vector<char> articleText_;
  std::vector<size_t> pageStarts_;
  size_t selectedTopic_ = 0;
  size_t selectedScrap_ = 0;
  size_t currentPage_ = 0;
  bool loadPending_ = false;
  bool layoutOverflow_ = false;
  bool scrapsLoaded_ = false;
  bool scrapStoreLoadFailed_ = false;
  bool scrapStorageError_ = false;
  bool articleScrapped_ = false;
  FeedLoadError lastError_ = FeedLoadError::None;
  FeedLoadDiagnostics lastDiagnostics_;
  int pendingTopicId_ = 0;
  View articleBackView_ = View::Topics;
  std::string articleTitle_;
  std::string sourceUrl_;

  void connectWifi();
  void disconnectWifi();
  void releaseArticleLayout();
  void failLayout(FeedLoadDiagnostics::MemoryStage stage, size_t requestedBytes = 0,
                  FeedLoadError error = FeedLoadError::Memory);
  template <typename T>
  bool ensureLayoutCapacity(std::vector<T>& storage, size_t required, size_t limit, size_t step,
                             FeedLoadDiagnostics::MemoryStage stage);
  void loadTopics();
  void loadGeekNewsTopics();
  void loadHackerNewsTopics();
  void loadArticle(int topicId);
  bool loadGeekNewsArticle(int topicId);
  bool loadHackerNewsArticle(int topicId);
  void retryLoad();
  bool ensureScrapsLoaded();
  void openScraps();
  void openSelectedScrap();
  void showSelectedScrapQr();
  void scrapArticle();
  void deleteSelectedScrap();
  void showArticleQr();
  void layoutMarkdown(const std::string& markdown, const std::string& explicitSourceUrl = {});
  void appendMarkdownBlock(const std::string& text, const std::string& prefix, int indent, bool quote,
                           bool codeBlock, int spacingAfter);
  void appendWrappedSpans(const std::vector<Span>& spans, const std::string& prefix, int indent, bool quote,
                          int spacingAfter);
  bool storeLayoutLine(RichLine line, const std::vector<Span>& spans = {});
  void rebuildPageStarts();
  void drawSources();
  void drawTopics();
  void drawArticle();
  void drawScraps();
  void drawDeleteConfirmation();
  void drawStatus(const char* message, bool retry);
  const char* sourceName() const;
  const char* loadErrorMessage() const;

  static bool receiveFeedChunk(void* context, const uint8_t* data, size_t length);
  static bool parseFeedEntry(const std::string& entry, Topic& topic);
  static std::vector<Span> parseInline(const std::string& text, bool codeBlock = false);
  static std::string hackerNewsHtmlToMarkdown(const std::string& html);
  static std::string decodeEntities(const std::string& text);
  static std::string trim(const std::string& text);
};
