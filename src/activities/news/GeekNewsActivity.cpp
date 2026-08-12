#include "GeekNewsActivity.h"

#include <I18n.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kHost = "news.hada.io";
constexpr const char* kFeedPath = "/rss/news";
constexpr const char* kTopicPathPrefix = "/topic/";
constexpr size_t kMaxArticleBytes = 48 * 1024;
constexpr size_t kMaxFeedEntryBytes = 16 * 1024;
constexpr uint32_t kNetworkTimeoutMs = 15000;
using ResponseWriter = bool (*)(void*, const uint8_t*, size_t);

bool readResponseBytes(NetworkClientSecure& client, size_t byteCount, void* context, ResponseWriter writer) {
  uint8_t buffer[512];
  while (byteCount > 0) {
    const size_t requested = std::min(byteCount, sizeof(buffer));
    const size_t received = client.readBytes(buffer, requested);
    if (received == 0) return false;
    if (!writer(context, buffer, received)) return false;
    byteCount -= received;
  }
  return true;
}

bool fetchGeekNews(const std::string& path, void* context, ResponseWriter writer) {
  NetworkClientSecure client;
  // 공개 읽기 전용 콘텐츠이며, 전체 CA 번들은 ESP32-C3의 DROM 한도를 초과한다.
  client.setInsecure();
  client.setTimeout(kNetworkTimeoutMs);
  if (!client.connect(kHost, 443, static_cast<int32_t>(kNetworkTimeoutMs))) return false;

  client.print("GET ");
  client.print(path.c_str());
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(kHost);
  client.print("\r\nUser-Agent: CrossPoint/1.0 (+https://github.com/An-d-u/crosspoint-reader-ko)"
               "\r\nAccept: text/markdown, application/atom+xml, text/plain;q=0.9"
               "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");

  String line = client.readStringUntil('\n');
  line.trim();
  if (!line.startsWith("HTTP/1.1 200") && !line.startsWith("HTTP/1.0 200")) return false;

  int64_t contentLength = -1;
  bool chunked = false;
  while (true) {
    line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lowered = line;
    lowered.toLowerCase();
    if (lowered.startsWith("content-length:")) {
      contentLength = lowered.substring(15).toInt();
    } else if (lowered.startsWith("transfer-encoding:") && lowered.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }

  if (chunked) {
    while (true) {
      line = client.readStringUntil('\n');
      line.trim();
      const int separator = line.indexOf(';');
      if (separator >= 0) line.remove(separator);
      const size_t chunkSize = std::strtoul(line.c_str(), nullptr, 16);
      if (chunkSize == 0) return true;
      if (!readResponseBytes(client, chunkSize, context, writer)) return false;
      uint8_t newline[2];
      if (client.readBytes(newline, sizeof(newline)) != sizeof(newline)) return false;
    }
  }

  if (contentLength >= 0) {
    return readResponseBytes(client, static_cast<size_t>(contentLength), context, writer);
  }

  uint8_t buffer[512];
  uint32_t lastReceivedAt = millis();
  while (client.connected() || client.available() > 0) {
    const int available = client.available();
    if (available <= 0) {
      if (millis() - lastReceivedAt > kNetworkTimeoutMs) return false;
      delay(5);
      continue;
    }
    const size_t requested = std::min(static_cast<size_t>(available), sizeof(buffer));
    const int received = client.read(buffer, requested);
    if (received <= 0) continue;
    if (!writer(context, buffer, static_cast<size_t>(received))) return false;
    lastReceivedAt = millis();
  }
  return true;
}

struct StringResponseContext {
  std::string* output;
  size_t limit;
};

bool receiveStringChunk(void* rawContext, const uint8_t* data, const size_t length) {
  auto& context = *static_cast<StringResponseContext*>(rawContext);
  if (context.output->size() > context.limit || length > context.limit - context.output->size()) return false;
  context.output->append(reinterpret_cast<const char*>(data), length);
  return true;
}

bool fetchGeekNewsArticle(const std::string& path, std::string& output) {
  output.clear();
  StringResponseContext context{&output, kMaxArticleBytes};
  return fetchGeekNews(path, &context, receiveStringChunk) && !output.empty();
}

std::string unwrapCdata(std::string value) {
  const size_t start = value.find("<![CDATA[");
  const size_t end = value.rfind("]]>");
  if (start != std::string::npos && end != std::string::npos && end >= start + 9) {
    return value.substr(start + 9, end - start - 9);
  }
  return value;
}

std::string elementText(const std::string& xml, const char* name) {
  const std::string opening = std::string("<") + name;
  const size_t tag = xml.find(opening);
  if (tag == std::string::npos) return {};
  const size_t content = xml.find('>', tag + opening.size());
  if (content == std::string::npos) return {};
  const std::string closing = std::string("</") + name + ">";
  const size_t end = xml.find(closing, content + 1);
  if (end == std::string::npos) return {};
  return unwrapCdata(xml.substr(content + 1, end - content - 1));
}

size_t utf8CharBytes(const unsigned char first) {
  if ((first & 0x80) == 0) return 1;
  if ((first & 0xE0) == 0xC0) return 2;
  if ((first & 0xF0) == 0xE0) return 3;
  if ((first & 0xF8) == 0xF0) return 4;
  return 1;
}
}  // namespace

void GeekNewsActivity::onEnter() {
  Activity::onEnter();
  connectWifi();
}

void GeekNewsActivity::onExit() {
  disconnectWifi();
  topics_.clear();
  articleLines_.clear();
  pageStarts_.clear();
  Activity::onExit();
}

void GeekNewsActivity::connectWifi() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             onGoHome();
                             return;
                           }
                           WiFi.setSleep(false);
                           view_ = View::LoadingTopics;
                           loadPending_ = true;
                           requestUpdate();
                         });
}

void GeekNewsActivity::disconnectWifi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

std::string GeekNewsActivity::trim(const std::string& text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(start, end - start);
}

std::string GeekNewsActivity::decodeEntities(const std::string& text) {
  std::string output;
  output.reserve(text.size());
  for (size_t index = 0; index < text.size();) {
    if (text[index] != '&') {
      output.push_back(text[index++]);
      continue;
    }
    const size_t end = text.find(';', index + 1);
    if (end == std::string::npos || end - index > 12) {
      output.push_back(text[index++]);
      continue;
    }
    const std::string entity = text.substr(index, end - index + 1);
    if (entity == "&amp;") {
      output.push_back('&');
    } else if (entity == "&lt;") {
      output.push_back('<');
    } else if (entity == "&gt;") {
      output.push_back('>');
    } else if (entity == "&quot;") {
      output.push_back('"');
    } else if (entity == "&apos;") {
      output.push_back('\'');
    } else if (entity.size() > 3 && entity[1] == '#') {
      char* parsedEnd = nullptr;
      const bool hexadecimal = entity.size() > 4 && (entity[2] == 'x' || entity[2] == 'X');
      const char* number = entity.c_str() + (hexadecimal ? 3 : 2);
      const uint32_t value = static_cast<uint32_t>(std::strtoul(number, &parsedEnd, hexadecimal ? 16 : 10));
      if (parsedEnd != nullptr && *parsedEnd == ';' && value <= 0x10FFFF) {
        if (value <= 0x7F) {
          output.push_back(static_cast<char>(value));
        } else if (value <= 0x7FF) {
          output.push_back(static_cast<char>(0xC0 | (value >> 6)));
          output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value <= 0xFFFF) {
          output.push_back(static_cast<char>(0xE0 | (value >> 12)));
          output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
          output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
          output.push_back(static_cast<char>(0xF0 | (value >> 18)));
          output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
          output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
          output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
      } else {
        output += entity;
      }
    } else {
      output += entity;
    }
    index = end + 1;
  }
  return output;
}

bool GeekNewsActivity::parseFeedEntry(const std::string& entry, Topic& topic) {
  const size_t topicMarker = entry.find("topic?id=");
  if (topicMarker == std::string::npos) return false;
  const int id = std::atoi(entry.c_str() + topicMarker + 9);
  std::string title = trim(decodeEntities(elementText(entry, "title")));
  if (id <= 0 || title.empty()) return false;

  std::string author;
  const size_t authorStart = entry.find("<author>");
  const size_t authorEnd = entry.find("</author>", authorStart);
  if (authorStart != std::string::npos && authorEnd != std::string::npos) {
    author = trim(decodeEntities(elementText(entry.substr(authorStart, authorEnd - authorStart), "name")));
  }
  std::string published = trim(elementText(entry, "published"));
  if (published.size() >= 10) published.resize(10);

  topic.id = id;
  topic.title = std::move(title);
  topic.subtitle = std::move(author);
  if (!published.empty()) {
    if (!topic.subtitle.empty()) topic.subtitle += " · ";
    topic.subtitle += published;
  }
  return true;
}

bool GeekNewsActivity::receiveFeedChunk(void* rawContext, const uint8_t* data, const size_t length) {
  auto& context = *static_cast<FeedStreamContext*>(rawContext);
  if (context.topics->size() >= kMaxTopics) return true;
  context.pending.append(reinterpret_cast<const char*>(data), length);

  while (context.topics->size() < kMaxTopics) {
    const size_t start = context.pending.find("<entry>");
    if (start == std::string::npos) {
      if (context.pending.size() > 16) context.pending.erase(0, context.pending.size() - 16);
      return true;
    }
    const size_t end = context.pending.find("</entry>", start + 7);
    if (end == std::string::npos) {
      if (start > 0) context.pending.erase(0, start);
      return context.pending.size() <= kMaxFeedEntryBytes;
    }

    Topic topic;
    if (parseFeedEntry(context.pending.substr(start, end + 8 - start), topic)) {
      context.topics->push_back(std::move(topic));
    }
    context.pending.erase(0, end + 8);
  }
  context.pending.clear();
  return true;
}

void GeekNewsActivity::loadTopics() {
  topics_.clear();
  FeedStreamContext context;
  context.pending.reserve(2048);
  context.topics = &topics_;
  if (!fetchGeekNews(kFeedPath, &context, receiveFeedChunk) || topics_.empty()) {
    view_ = View::Error;
    requestUpdate();
    return;
  }
  selectedTopic_ = 0;
  view_ = View::Topics;
  requestUpdate();
}

void GeekNewsActivity::loadArticle(const int topicId) {
  std::string markdown;
  const std::string path = std::string(kTopicPathPrefix) + std::to_string(topicId) + ".md";
  if (!fetchGeekNewsArticle(path, markdown)) {
    view_ = View::Error;
    requestUpdate();
    return;
  }
  layoutMarkdown(markdown);
  if (articleLines_.empty()) {
    view_ = View::Error;
  } else {
    currentPage_ = 0;
    view_ = View::Article;
  }
  requestUpdate();
}

void GeekNewsActivity::retryLoad() {
  if (pendingTopicId_ > 0) {
    view_ = View::LoadingArticle;
  } else {
    view_ = View::LoadingTopics;
  }
  loadPending_ = true;
  requestUpdate();
}

std::vector<GeekNewsActivity::Span> GeekNewsActivity::parseInline(const std::string& text, const bool codeBlock) {
  std::vector<Span> spans;
  auto append = [&spans](const std::string& value, const InlineStyle style) {
    if (value.empty()) return;
    if (!spans.empty() && spans.back().style == style) {
      spans.back().text += value;
    } else {
      spans.push_back({value, style});
    }
  };
  if (codeBlock) {
    append(decodeEntities(text), InlineStyle::Code);
    return spans;
  }

  const std::string decoded = decodeEntities(text);

  bool bold = false;
  bool italic = false;
  bool code = false;
  for (size_t index = 0; index < decoded.size();) {
    if (decoded[index] == '\\' && index + 1 < decoded.size()) {
      append(decoded.substr(index + 1, 1), code ? InlineStyle::Code
                                            : (bold ? InlineStyle::Bold
                                                    : (italic ? InlineStyle::Italic : InlineStyle::Regular)));
      index += 2;
      continue;
    }
    if (!code && index + 1 < decoded.size() && decoded.compare(index, 2, "**") == 0) {
      bold = !bold;
      index += 2;
      continue;
    }
    if (decoded[index] == '`') {
      code = !code;
      ++index;
      continue;
    }
    if (!code && decoded[index] == '*') {
      italic = !italic;
      ++index;
      continue;
    }
    const bool image = !code && index + 1 < decoded.size() && decoded[index] == '!' && decoded[index + 1] == '[';
    const size_t labelStart = image ? index + 2 : index + 1;
    if (!code && (image || decoded[index] == '[')) {
      const size_t labelEnd = decoded.find("](", labelStart);
      const size_t urlEnd = labelEnd == std::string::npos ? std::string::npos : decoded.find(')', labelEnd + 2);
      if (labelEnd != std::string::npos && urlEnd != std::string::npos) {
        const std::string label = decoded.substr(labelStart, labelEnd - labelStart);
        append(image ? std::string("[") + label + "]" : label, InlineStyle::Link);
        index = urlEnd + 1;
        continue;
      }
    }
    if (!code && decoded[index] == '<') {
      const size_t tagEnd = decoded.find('>', index + 1);
      if (tagEnd != std::string::npos) {
        index = tagEnd + 1;
        continue;
      }
    }

    const size_t bytes =
        std::min(utf8CharBytes(static_cast<unsigned char>(decoded[index])), decoded.size() - index);
    const InlineStyle style = code ? InlineStyle::Code
                                   : (bold ? InlineStyle::Bold
                                           : (italic ? InlineStyle::Italic : InlineStyle::Regular));
    append(decoded.substr(index, bytes), style);
    index += bytes;
  }
  return spans;
}

void GeekNewsActivity::appendWrappedSpans(const std::vector<Span>& spans, const std::string& prefix, const int indent,
                                          const bool quote, const int spacingAfter) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId) + 5;
  const int pageWidth = renderer.getScreenWidth();
  const int prefixWidth =
      prefix.empty() ? 0 : renderer.getTextWidth(fontId, prefix.c_str(), EpdFontFamily::BOLD);

  RichLine line;
  line.indent = indent;
  line.height = lineHeight;
  line.quote = quote;
  int width = 0;
  bool hasBodyText = false;
  if (!prefix.empty()) {
    line.spans.push_back({prefix, InlineStyle::Bold});
    width = prefixWidth;
  }

  auto styleFor = [](const InlineStyle style) {
    if (style == InlineStyle::Bold) return EpdFontFamily::BOLD;
    if (style == InlineStyle::Italic) return EpdFontFamily::ITALIC;
    return EpdFontFamily::REGULAR;
  };
  auto pushLine = [&]() {
    articleLines_.push_back(std::move(line));
    line = RichLine{};
    line.indent = indent + prefixWidth;
    line.height = lineHeight;
    line.quote = quote;
    width = 0;
    hasBodyText = false;
  };

  for (const Span& span : spans) {
    for (size_t index = 0; index < span.text.size();) {
      const size_t bytes = std::min(utf8CharBytes(static_cast<unsigned char>(span.text[index])),
                                    span.text.size() - index);
      const std::string character = span.text.substr(index, bytes);
      const int characterWidth = renderer.getTextWidth(fontId, character.c_str(), styleFor(span.style));
      const int available = pageWidth - metrics.contentSidePadding * 2 - line.indent - (quote ? 10 : 0);
      if (hasBodyText && width + characterWidth > available) pushLine();
      if (!line.spans.empty() && line.spans.back().style == span.style) {
        line.spans.back().text += character;
      } else {
        line.spans.push_back({character, span.style});
      }
      width += characterWidth;
      hasBodyText = true;
      index += bytes;
    }
  }
  if (!line.spans.empty()) articleLines_.push_back(std::move(line));
  if (spacingAfter > 0) {
    RichLine space;
    space.height = spacingAfter;
    articleLines_.push_back(std::move(space));
  }
}

void GeekNewsActivity::appendMarkdownBlock(const std::string& text, const std::string& prefix, const int indent,
                                           const bool quote, const bool codeBlock, const int spacingAfter) {
  appendWrappedSpans(parseInline(text, codeBlock), prefix, indent, quote, spacingAfter);
}

void GeekNewsActivity::layoutMarkdown(const std::string& markdown) {
  articleLines_.clear();
  pageStarts_.clear();
  sourceUrl_.clear();

  const size_t sourceMarker = markdown.find("- Original source:");
  if (sourceMarker != std::string::npos) {
    const size_t urlStart = markdown.find("](", sourceMarker);
    const size_t urlEnd = urlStart == std::string::npos ? std::string::npos : markdown.find(')', urlStart + 2);
    if (urlStart != std::string::npos && urlEnd != std::string::npos) {
      sourceUrl_ = markdown.substr(urlStart + 2, urlEnd - urlStart - 2);
    }
  }

  appendMarkdownBlock("**" + articleTitle_ + "**", "", 0, false, false, 12);
  if (!sourceUrl_.empty()) {
    appendMarkdownBlock(std::string(tr(STR_GEEKNEWS_ORIGINAL)) + " [" + sourceUrl_ + "](" + sourceUrl_ + ")", "",
                        0, false, false, 12);
  }

  size_t bodyStart = markdown.find("## Topic Body");
  if (bodyStart != std::string::npos) bodyStart = markdown.find('\n', bodyStart);
  if (bodyStart == std::string::npos) bodyStart = 0;
  size_t bodyEnd = markdown.find("\n## Comments", bodyStart);
  if (bodyEnd == std::string::npos) bodyEnd = markdown.size();
  const std::string_view body(markdown.data() + bodyStart, bodyEnd - bodyStart);

  bool inCodeFence = false;
  size_t cursor = 0;
  while (cursor <= body.size()) {
    const size_t end = body.find('\n', cursor);
    std::string line(body.substr(cursor, end == std::string_view::npos ? std::string_view::npos : end - cursor));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    cursor = end == std::string_view::npos ? body.size() + 1 : end + 1;

    if (trim(line).rfind("```", 0) == 0) {
      inCodeFence = !inCodeFence;
      if (!inCodeFence) {
        RichLine space;
        space.height = 8;
        articleLines_.push_back(std::move(space));
      }
      continue;
    }
    if (inCodeFence) {
      appendMarkdownBlock(line.empty() ? " " : line, "", 12, false, true, 2);
      continue;
    }

    const std::string stripped = trim(line);
    if (stripped.empty()) {
      if (!articleLines_.empty() && articleLines_.back().height > 0 && !articleLines_.back().spans.empty()) {
        RichLine space;
        space.height = 8;
        articleLines_.push_back(std::move(space));
      }
      continue;
    }
    if (stripped == "---" || stripped == "***" || stripped == "___") {
      RichLine rule;
      rule.height = 14;
      rule.rule = true;
      articleLines_.push_back(std::move(rule));
      continue;
    }

    size_t leadingSpaces = 0;
    while (leadingSpaces < line.size() && line[leadingSpaces] == ' ') ++leadingSpaces;
    const int nestedIndent = static_cast<int>((leadingSpaces / 2) * 16);
    std::string content = line.substr(leadingSpaces);

    size_t heading = 0;
    while (heading < content.size() && content[heading] == '#') ++heading;
    if (heading > 0 && heading <= 6 && heading < content.size() && content[heading] == ' ') {
      appendMarkdownBlock("**" + trim(content.substr(heading + 1)) + "**", heading <= 3 ? "■ " : "", 0, false,
                          false, heading <= 2 ? 14 : 9);
      continue;
    }
    if (content.rfind("> ", 0) == 0 || content == ">") {
      appendMarkdownBlock(content.size() > 2 ? content.substr(2) : " ", "", nestedIndent + 10, true, false, 6);
      continue;
    }
    if (content.size() >= 2 && (content[0] == '-' || content[0] == '*' || content[0] == '+') && content[1] == ' ') {
      appendMarkdownBlock(content.substr(2), "• ", nestedIndent, false, false, 3);
      continue;
    }
    size_t numberEnd = 0;
    while (numberEnd < content.size() && std::isdigit(static_cast<unsigned char>(content[numberEnd]))) ++numberEnd;
    if (numberEnd > 0 && numberEnd + 1 < content.size() && content[numberEnd] == '.' &&
        content[numberEnd + 1] == ' ') {
      appendMarkdownBlock(content.substr(numberEnd + 2), content.substr(0, numberEnd + 2), nestedIndent, false, false,
                          3);
      continue;
    }
    if (content.size() >= 2 && content.front() == '|' && content.back() == '|') {
      content = content.substr(1, content.size() - 2);
      std::replace(content.begin(), content.end(), '|', '/');
    }
    appendMarkdownBlock(content, "", nestedIndent, false, false, 6);
  }
  rebuildPageStarts();
}

void GeekNewsActivity::rebuildPageStarts() {
  pageStarts_.clear();
  pageStarts_.push_back(0);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int availableHeight = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight -
                              metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int used = 0;
  for (size_t index = 0; index < articleLines_.size(); ++index) {
    const int height = articleLines_[index].height;
    if (used > 0 && used + height > availableHeight) {
      pageStarts_.push_back(index);
      used = 0;
    }
    used += height;
  }
}

void GeekNewsActivity::loop() {
  if (loadPending_) {
    loadPending_ = false;
    requestUpdateAndWait();
    if (view_ == View::LoadingArticle) {
      loadArticle(pendingTopicId_);
    } else {
      pendingTopicId_ = 0;
      loadTopics();
    }
    return;
  }

  if (view_ == View::LoadingTopics || view_ == View::LoadingArticle) return;
  if (view_ == View::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      retryLoad();
    }
    return;
  }
  if (view_ == View::Topics) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && selectedTopic_ < topics_.size()) {
      pendingTopicId_ = topics_[selectedTopic_].id;
      articleTitle_ = topics_[selectedTopic_].title;
      view_ = View::LoadingArticle;
      loadPending_ = true;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      pendingTopicId_ = 0;
      view_ = View::LoadingTopics;
      loadPending_ = true;
      requestUpdate();
      return;
    }
    const int count = static_cast<int>(topics_.size());
    const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
    buttonNavigator_.onNextRelease([this, count] {
      selectedTopic_ = ButtonNavigator::nextIndex(static_cast<int>(selectedTopic_), count);
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this, count] {
      selectedTopic_ = ButtonNavigator::previousIndex(static_cast<int>(selectedTopic_), count);
      requestUpdate();
    });
    buttonNavigator_.onNextContinuous([this, count, pageItems] {
      selectedTopic_ = ButtonNavigator::nextPageIndex(static_cast<int>(selectedTopic_), count, pageItems);
      requestUpdate();
    });
    buttonNavigator_.onPreviousContinuous([this, count, pageItems] {
      selectedTopic_ = ButtonNavigator::previousPageIndex(static_cast<int>(selectedTopic_), count, pageItems);
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    view_ = View::Topics;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) && currentPage_ > 0) {
    --currentPage_;
    requestUpdate();
  } else if ((mappedInput.wasReleased(MappedInputManager::Button::Right) ||
              mappedInput.wasReleased(MappedInputManager::Button::Confirm)) &&
             currentPage_ + 1 < pageStarts_.size()) {
    ++currentPage_;
    requestUpdate();
  }
}

void GeekNewsActivity::drawStatus(const char* message, const bool retry) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GEEKNEWS));
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, message, true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), retry ? tr(STR_RETRY) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawTopics() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GEEKNEWS));
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, topics_.size(), selectedTopic_,
               [this](const int index) { return topics_[index].title; },
               [this](const int index) { return topics_[index].subtitle; }, nullptr, nullptr);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawArticle() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int fontId = SETTINGS.getReaderFontId();
  char pageLabel[24];
  std::snprintf(pageLabel, sizeof(pageLabel), "%zu/%zu", currentPage_ + 1, pageStarts_.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GEEKNEWS), pageLabel);

  const size_t start = pageStarts_.empty() ? 0 : pageStarts_[currentPage_];
  const size_t end = currentPage_ + 1 < pageStarts_.size() ? pageStarts_[currentPage_ + 1] : articleLines_.size();
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  for (size_t index = start; index < end; ++index) {
    const RichLine& line = articleLines_[index];
    if (line.rule) {
      renderer.drawLine(metrics.contentSidePadding, y + line.height / 2,
                        renderer.getScreenWidth() - metrics.contentSidePadding, y + line.height / 2);
      y += line.height;
      continue;
    }
    if (line.spans.empty()) {
      y += line.height;
      continue;
    }
    int x = metrics.contentSidePadding + line.indent + (line.quote ? 10 : 0);
    if (line.quote) renderer.drawLine(x - 8, y, x - 8, y + line.height - 2, 2, true);
    for (const Span& span : line.spans) {
      EpdFontFamily::Style style = EpdFontFamily::REGULAR;
      if (span.style == InlineStyle::Bold) style = EpdFontFamily::BOLD;
      if (span.style == InlineStyle::Italic) style = EpdFontFamily::ITALIC;
      const int width = renderer.getTextWidth(fontId, span.text.c_str(), style);
      if (span.style == InlineStyle::Code) renderer.drawRect(x - 1, y - 1, width + 2, line.height - 2);
      renderer.drawText(fontId, x, y, span.text.c_str(), true, style);
      if (span.style == InlineStyle::Link) renderer.drawLine(x, y + line.height - 4, x + width, y + line.height - 4);
      x += width;
    }
    y += line.height;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_PAGE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (view_ == View::LoadingTopics || view_ == View::LoadingArticle) {
    drawStatus(tr(STR_LOADING), false);
  } else if (view_ == View::Error) {
    drawStatus(tr(STR_GEEKNEWS_LOAD_FAILED), true);
  } else if (view_ == View::Topics) {
    drawTopics();
  } else {
    drawArticle();
  }
  renderer.displayBuffer();
}
