#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GeekNewsActivity final : public Activity {
 public:
  explicit GeekNewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GeekNews", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class View : uint8_t { LoadingTopics, Topics, LoadingArticle, Article, Error };
  enum class InlineStyle : uint8_t { Regular, Bold, Italic, Code, Link };

  struct Topic {
    int id = 0;
    std::string title;
    std::string subtitle;
  };

  struct Span {
    std::string text;
    InlineStyle style = InlineStyle::Regular;
  };

  struct RichLine {
    std::vector<Span> spans;
    int indent = 0;
    int height = 0;
    bool quote = false;
    bool rule = false;
  };

  struct FeedStreamContext {
    std::string pending;
    std::vector<Topic>* topics = nullptr;
  };

  static constexpr int kMaxTopics = 20;

  ButtonNavigator buttonNavigator_;
  View view_ = View::LoadingTopics;
  std::vector<Topic> topics_;
  std::vector<RichLine> articleLines_;
  std::vector<size_t> pageStarts_;
  size_t selectedTopic_ = 0;
  size_t currentPage_ = 0;
  bool loadPending_ = false;
  int pendingTopicId_ = 0;
  std::string articleTitle_;
  std::string sourceUrl_;

  void connectWifi();
  void disconnectWifi();
  void loadTopics();
  void loadArticle(int topicId);
  void retryLoad();
  void layoutMarkdown(const std::string& markdown);
  void appendMarkdownBlock(const std::string& text, const std::string& prefix, int indent, bool quote,
                           bool codeBlock, int spacingAfter);
  void appendWrappedSpans(const std::vector<Span>& spans, const std::string& prefix, int indent, bool quote,
                          int spacingAfter);
  void rebuildPageStarts();
  void drawTopics();
  void drawArticle();
  void drawStatus(const char* message, bool retry);

  static bool receiveFeedChunk(void* context, const uint8_t* data, size_t length);
  static bool parseFeedEntry(const std::string& entry, Topic& topic);
  static std::vector<Span> parseInline(const std::string& text, bool codeBlock = false);
  static std::string decodeEntities(const std::string& text);
  static std::string trim(const std::string& text);
};
