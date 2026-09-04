#include "GeekNewsActivity.h"
#include "FeedLayoutCapacity.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FeedSecureClient.h"

namespace {
constexpr const char* kHost = "news.hada.io";
constexpr const char* kHackerNewsHost = "hn.algolia.com";
constexpr const char* kHackerNewsFeedPath = "/api/v1/search?tags=front_page&hitsPerPage=20";
constexpr const char* kExtractorHost = "r.jina.ai";
constexpr const char* kFeedPath = "/rss/news";
constexpr const char* kHackerNewsTempPath = "/.crosspoint/hackernews_front.tmp";
constexpr const char* kArticleTempPath = "/.crosspoint/feed_article.tmp";
constexpr size_t kMaxArticleBytes = 48 * 1024;
constexpr size_t kMaxHackerNewsFeedBytes = 48 * 1024;
constexpr size_t kMaxArticleLineBytes = 4 * 1024;
constexpr size_t kMaxLayoutLines = 8192;
constexpr size_t kMaxLayoutSpans = 8192;
constexpr size_t kMaxLayoutTextBytes = UINT16_MAX;
constexpr size_t kMaxFeedEntryBytes = 16 * 1024;
constexpr uint32_t kNetworkTimeoutMs = 30000;
constexpr uint32_t kExtractorTimeoutMs = 60000;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kNetworkSettleMs = 300;
constexpr int kNetworkAttempts = 3;
constexpr int kMaxRedirects = 5;
constexpr uint32_t kRetryDelayMs = 750;
constexpr int kTextRightSafetyPx = 2;
constexpr size_t kAllocationSafetyBytes = 8 * 1024;
using ResponseWriter = bool (*)(void*, const uint8_t*, size_t);

bool hasAllocationRoom(const size_t bytes) {
  if (bytes == 0) return true;
  const size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  return freeHeap > bytes + kAllocationSafetyBytes && largestBlock > bytes + 256;
}

void recordMemoryFailure(FeedLoadDiagnostics& diagnostics, const FeedLoadDiagnostics::MemoryStage stage,
                         const size_t requestedBytes) {
  diagnostics.memoryStage = stage;
  diagnostics.requestedBytes = requestedBytes;
  diagnostics.freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  diagnostics.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

void releaseArticleWifi() {
  if (WiFi.getMode() == WIFI_OFF) return;
  // 저장된 접속 정보가 없는 네트워크는 유지해 재시도가 불가능해지는 것을 막는다.
  const std::string reconnectSsid = WIFI_STORE.getLastConnectedSsid();
  if (reconnectSsid.empty() || WIFI_STORE.findCredential(reconnectSsid) == nullptr) return;
  WiFi.disconnect(false);
  delay(50);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool hasAddress(const IPAddress& address) {
  const uint32_t value = static_cast<uint32_t>(address);
  return value != 0 && value != UINT32_MAX;
}

bool wifiNetworkReady() {
  return WiFi.status() == WL_CONNECTED && hasAddress(WiFi.localIP()) && hasAddress(WiFi.gatewayIP()) &&
         hasAddress(WiFi.dnsIP());
}

bool waitForWifi() {
  if (wifiNetworkReady()) {
    WiFi.setSleep(false);
    return true;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  const auto* credential = WIFI_STORE.findCredential(ssid);
  if (credential != nullptr) {
    if (credential->password.empty()) {
      WiFi.begin(credential->ssid.c_str());
    } else {
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    }
  } else {
    WiFi.reconnect();
  }
  const uint32_t startedAt = millis();
  while (!wifiNetworkReady() && millis() - startedAt < kWifiConnectTimeoutMs) {
    delay(100);
  }
  const bool connected = wifiNetworkReady();
  if (connected) {
    WiFi.setSleep(false);
    delay(kNetworkSettleMs);
  }
  return connected;
}

class ResponseWriterStream final : public Stream {
 public:
  ResponseWriterStream(void* context, const ResponseWriter writer) : context_(context), writer_(writer) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    if (failed_ || !writer_(context_, buffer, size)) {
      failed_ = true;
      return 0;
    }
    received_ += size;
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool failed() const { return failed_; }
  size_t received() const { return received_; }

 private:
  void* context_ = nullptr;
  ResponseWriter writer_ = nullptr;
  size_t received_ = 0;
  bool failed_ = false;
};

FeedLoadError classifyConnectionFailure(const char* host, const int httpError) {
  if (httpError == HTTPC_ERROR_TOO_LESS_RAM) return FeedLoadError::Memory;
  if (!wifiNetworkReady()) return FeedLoadError::Wifi;
  if (httpError == HTTPC_ERROR_READ_TIMEOUT || httpError == HTTPC_ERROR_CONNECTION_LOST) {
    return FeedLoadError::Response;
  }
  IPAddress address;
  if (WiFi.hostByName(host, address) != 1 || !hasAddress(address)) return FeedLoadError::Dns;
  return FeedLoadError::Tls;
}

FeedLoadError fetchHttps(const char* host, const std::string& path, const char* accept, void* context,
                         ResponseWriter writer, FeedLoadDiagnostics& diagnostics,
                         const uint32_t timeoutMs = kNetworkTimeoutMs) {
  diagnostics = {};
  if (!waitForWifi()) return FeedLoadError::Wifi;

  const std::string url = std::string("https://") + host + path;
  FeedSecureClient client;
  // 기존 동작 유지: 부팅 직후 공개 읽기 요청에서도 인증서 검증 전체를 생략한다(서버 신원은 검증되지 않음).
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url.c_str())) return FeedLoadError::Http;
  http.setConnectTimeout(static_cast<int32_t>(timeoutMs));
  http.setTimeout(static_cast<uint16_t>(timeoutMs));
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(kMaxRedirects);
  http.setUserAgent("CrossPoint/1.0 (+https://github.com/An-d-u/crosspoint-reader-ko)");
  http.setAcceptEncoding("identity");
  http.addHeader("Accept", accept);

  const uint32_t startedAt = millis();
  diagnostics.stage = FeedLoadDiagnostics::Stage::Headers;
  const int status = http.GET();
  diagnostics.code = status;
  diagnostics.elapsedMs = millis() - startedAt;
  if (status != HTTP_CODE_OK) {
    const FeedLoadError error =
        status < 0 ? classifyConnectionFailure(host, status) : FeedLoadError::Http;
    http.end();
    return error;
  }

  const int expected = http.getSize();
  diagnostics.stage = FeedLoadDiagnostics::Stage::Body;
  diagnostics.expected = expected;
  ResponseWriterStream output(context, writer);
  const int received = http.writeToStream(&output);
  diagnostics.code = received < 0 ? received : status;
  diagnostics.received = output.received();
  diagnostics.sinkFailed = output.failed();
  diagnostics.timedOut = client.readTimedOut();
  diagnostics.elapsedMs = millis() - startedAt;
  const bool complete = received >= 0 && static_cast<size_t>(received) == output.received() && !output.failed() &&
                        !diagnostics.timedOut &&
                        (expected < 0 || output.received() == static_cast<size_t>(expected));
  http.end();
  if (received == HTTPC_ERROR_TOO_LESS_RAM) return FeedLoadError::Memory;
  return complete ? FeedLoadError::None : FeedLoadError::Response;
}

FeedLoadError fetchGeekNews(const std::string& path, void* context, ResponseWriter writer,
                           FeedLoadDiagnostics& diagnostics) {
  return fetchHttps(kHost, path, "text/markdown, application/atom+xml, text/plain;q=0.9", context, writer, diagnostics);
}

void prepareNetworkRetry(const FeedLoadError error) {
  if (error != FeedLoadError::Wifi && error != FeedLoadError::Dns && error != FeedLoadError::Tls) return;
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_STA);
}

struct FileResponseContext {
  HalFile* file = nullptr;
  size_t received = 0;
  size_t limit = 0;
  bool tooLarge = false;
  bool writeFailed = false;
};

bool receiveFileChunk(void* rawContext, const uint8_t* data, const size_t length) {
  auto& context = *static_cast<FileResponseContext*>(rawContext);
  if (context.received > context.limit || length > context.limit - context.received) {
    context.tooLarge = true;
    return false;
  }
  if (context.file->write(data, length) != length) {
    context.writeFailed = true;
    return false;
  }
  context.received += length;
  return true;
}

struct GeekArticleResponseContext {
  HalFile* file = nullptr;
  std::string pending;
  std::string sourceUrl;
  size_t written = 0;
  size_t limit = 0;
  bool markdownStarted = false;
  bool titleFound = false;
  bool metadataSkipped = false;
  bool bodyStarted = false;
  bool bodyFinished = false;
  bool tooLarge = false;
  bool writeFailed = false;
};

bool writeArticleText(GeekArticleResponseContext& context, const std::string_view text, const bool newline) {
  const size_t appendSize = text.size() + (newline ? 1 : 0);
  if (context.written > context.limit || appendSize > context.limit - context.written) {
    context.tooLarge = true;
    return false;
  }
  if ((!text.empty() && context.file->write(text.data(), text.size()) != text.size()) ||
      (newline && context.file->write(static_cast<uint8_t>('\n')) != 1)) {
    context.writeFailed = true;
    return false;
  }
  context.written += appendSize;
  return true;
}

bool processGeekArticleLine(GeekArticleResponseContext& context, std::string_view line, const bool newline) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

  if (!context.markdownStarted) {
    context.markdownStarted = line == "Markdown Content:";
    return true;
  }

  if (!context.titleFound) {
    if (line.rfind("# ", 0) != 0) return true;
    context.titleFound = true;
    const size_t urlStart = line.find("](");
    const size_t urlEnd = urlStart == std::string_view::npos ? std::string_view::npos : line.find(')', urlStart + 2);
    if (urlStart != std::string_view::npos && urlEnd != std::string_view::npos) {
      context.sourceUrl.assign(line.substr(urlStart + 2, urlEnd - urlStart - 2));
    }
    return true;
  }

  if (!context.bodyStarted) {
    if (line.empty()) return true;
    if (!context.metadataSkipped) {
      context.metadataSkipped = true;
      if (line.find(" by [") != std::string_view::npos && line.find("favorite") != std::string_view::npos) {
        return true;
      }
    }
    if (!context.sourceUrl.empty()) {
      const std::string sourceLine = "- Original source: [source](" + context.sourceUrl + ")";
      if (!writeArticleText(context, sourceLine, true)) return false;
    }
    if (!writeArticleText(context, "## Topic Body", true)) return false;
    context.bodyStarted = true;
  }

  if (line == "## 댓글과 토론" || line.rfind("## 함께 보면 좋은 글", 0) == 0) {
    context.bodyFinished = true;
    return true;
  }
  return writeArticleText(context, line, newline);
}

bool receiveGeekArticleChunk(void* rawContext, const uint8_t* data, const size_t length) {
  auto& context = *static_cast<GeekArticleResponseContext*>(rawContext);
  if (context.bodyFinished) return true;

  // HTTP 수신 블록 전체가 아니라 실제 한 줄에만 길이 제한을 적용한다.
  const std::string_view incoming(reinterpret_cast<const char*>(data), length);
  size_t cursor = 0;
  while (cursor < incoming.size()) {
    const size_t end = incoming.find('\n', cursor);
    const bool newline = end != std::string_view::npos;
    const size_t count = (newline ? end : incoming.size()) - cursor;
    const size_t required = context.pending.size() + count;
    if (required > kMaxArticleLineBytes) {
      context.tooLarge = true;
      return false;
    }
    if (required > context.pending.capacity()) {
      const size_t capacity = std::min(kMaxArticleLineBytes, std::max(required, context.pending.capacity() * 2));
      if (!hasAllocationRoom(capacity + 1)) {
        context.tooLarge = true;
        return false;
      }
      context.pending.reserve(capacity);
    }
    context.pending.append(incoming.data() + cursor, count);
    if (!newline) break;
    if (!processGeekArticleLine(context, context.pending, true)) return false;
    context.pending.clear();
    if (context.bodyFinished) return true;
    cursor = end + 1;
  }
  return true;
}

bool readArticleFile(std::string& output, FeedLoadError& error, FeedLoadDiagnostics& diagnostics) {
  // 완전 수신과 파일 닫기가 확인된 뒤 TLS뿐 아니라 Wi-Fi 버퍼도 먼저 반환한다.
  releaseArticleWifi();
  HalFile input = Storage.open(kArticleTempPath, O_RDONLY);
  if (!input) {
    error = FeedLoadError::StorageIo;
    return false;
  }
  const size_t length = input.size();
  if (length == 0 || length > kMaxArticleBytes) {
    input.close();
    error = length > kMaxArticleBytes ? FeedLoadError::Memory : FeedLoadError::Response;
    return false;
  }
  if (!hasAllocationRoom(length + 1)) {
    recordMemoryFailure(diagnostics, FeedLoadDiagnostics::MemoryStage::ArticleBuffer, length + 1);
    input.close();
    error = FeedLoadError::Memory;
    return false;
  }
  output.resize(length);
  size_t received = 0;
  while (received < length) {
    const int count = input.read(output.data() + received, length - received);
    if (count <= 0) break;
    received += static_cast<size_t>(count);
  }
  const bool closed = input.close();
  if (received != length || !closed) {
    output.clear();
    error = FeedLoadError::StorageIo;
    return false;
  }
  error = FeedLoadError::None;
  return true;
}

bool fetchGeekNewsArticle(const int topicId, std::string& output, FeedLoadError& error,
                          FeedLoadDiagnostics& diagnostics) {
  error = FeedLoadError::None;
  Storage.mkdir("/.crosspoint");
  for (int attempt = 0; attempt < kNetworkAttempts; ++attempt) {
    Storage.remove(kArticleTempPath);
    output.clear();
    HalFile file = Storage.open(kArticleTempPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!file) {
      error = FeedLoadError::StorageIo;
      break;
    }
    GeekArticleResponseContext context;
    context.file = &file;
    context.limit = kMaxArticleBytes;
    context.pending.reserve(1024);
    const std::string path = "/https://news.hada.io/topic?id=" + std::to_string(topicId);
    const FeedLoadError fetchError =
        fetchHttps(kExtractorHost, path, "text/markdown, text/plain;q=0.9", &context, receiveGeekArticleChunk,
                   diagnostics, kExtractorTimeoutMs);
    bool finalLineProcessed = true;
    if (fetchError == FeedLoadError::None && !context.bodyFinished && !context.pending.empty()) {
      finalLineProcessed = processGeekArticleLine(context, context.pending, false);
    }
    file.flush();
    const bool closed = file.close();
    const bool responseReady =
        fetchError == FeedLoadError::None && finalLineProcessed && closed && context.bodyStarted && context.written > 0;
    if (responseReady) {
      if (readArticleFile(output, error, diagnostics)) {
        Storage.remove(kArticleTempPath);
        return true;
      }
    } else {
      error = context.tooLarge
                  ? FeedLoadError::Memory
                  : (context.writeFailed || !closed
                         ? FeedLoadError::StorageIo
                         : (fetchError == FeedLoadError::None ? FeedLoadError::Parse : fetchError));
    }
    Storage.remove(kArticleTempPath);
    if (responseReady || context.tooLarge || context.writeFailed) break;
    prepareNetworkRetry(error);
    if (attempt + 1 < kNetworkAttempts) delay(kRetryDelayMs * static_cast<uint32_t>(attempt + 1));
  }
  output.clear();
  return false;
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

bool endsWithFold(const std::string_view text, const std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  const size_t start = text.size() - suffix.size();
  for (size_t index = 0; index < suffix.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(text[start + index])) !=
        std::tolower(static_cast<unsigned char>(suffix[index]))) {
      return false;
    }
  }
  return true;
}

bool hackerNewsUrlCanBeArticle(std::string_view url) {
  const size_t cut = url.find_first_of("?#");
  if (cut != std::string_view::npos) url = url.substr(0, cut);
  static constexpr std::string_view suffixes[] = {".pdf", ".zip", ".gz",  ".mp3", ".mp4", ".png",
                                                  ".jpg", ".jpeg", ".gif", ".svg", ".webp"};
  return std::none_of(std::begin(suffixes), std::end(suffixes),
                      [url](const std::string_view suffix) { return endsWithFold(url, suffix); });
}

void splitExtractorResponse(std::string& response) {
  static constexpr std::string_view marker = "Markdown Content:";
  const size_t bodyStart = response.find(marker);
  if (bodyStart != std::string::npos) response.erase(0, bodyStart + marker.size());
}
}  // namespace

void GeekNewsActivity::onEnter() {
  Activity::onEnter();
  view_ = View::Sources;
  requestUpdate();
}

void GeekNewsActivity::onExit() {
  disconnectWifi();
  topics_.clear();
  releaseArticleLayout();
  Activity::onExit();
}

void GeekNewsActivity::connectWifi() {
  if (wifiNetworkReady()) {
    WiFi.setSleep(false);
    view_ = View::LoadingTopics;
    loadPending_ = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             view_ = View::Sources;
                             requestUpdate();
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

void GeekNewsActivity::releaseArticleLayout() {
  // clear()만으로는 용량이 반환되지 않아 다음 다운로드와 이전 기사 메모리가 겹친다.
  std::vector<RichLine>().swap(articleLines_);
  std::vector<StoredSpan>().swap(articleSpans_);
  std::vector<char>().swap(articleText_);
  std::vector<size_t>().swap(pageStarts_);
}

void GeekNewsActivity::failLayout(const FeedLoadDiagnostics::MemoryStage stage, const size_t requestedBytes,
                                 const FeedLoadError error) {
  layoutOverflow_ = true;
  const bool limit = stage == FeedLoadDiagnostics::MemoryStage::LineLimit ||
                     stage == FeedLoadDiagnostics::MemoryStage::SpanLimit ||
                     stage == FeedLoadDiagnostics::MemoryStage::TextLimit;
  lastError_ = limit ? FeedLoadError::LayoutLimit : error;
  recordMemoryFailure(lastDiagnostics_, stage, requestedBytes);
}

template <typename T>
bool GeekNewsActivity::ensureLayoutCapacity(std::vector<T>& storage, const size_t required, const size_t limit,
                                           const size_t step, const FeedLoadDiagnostics::MemoryStage stage) {
  const auto result = FeedLayoutCapacity::ensure(storage, required, limit, step, hasAllocationRoom);
  if (result.status == FeedLayoutCapacity::Status::Ready) return true;
  failLayout(stage, result.allocationBytes, result.status == FeedLayoutCapacity::Status::Limit
                                               ? FeedLoadError::LayoutLimit : FeedLoadError::Memory);
  return false;
}

std::string GeekNewsActivity::hackerNewsHtmlToMarkdown(const std::string& html) {
  std::string output;
  output.reserve(html.size());
  for (size_t index = 0; index < html.size();) {
    if (html[index] != '<') {
      output.push_back(html[index++]);
      continue;
    }
    const size_t close = html.find('>', index + 1);
    if (close == std::string::npos) {
      output.push_back(html[index++]);
      continue;
    }
    std::string tag;
    for (size_t cursor = index + 1; cursor < close; ++cursor) {
      const char value = html[cursor];
      if (std::isspace(static_cast<unsigned char>(value))) break;
      tag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }
    if (tag == "p" || tag == "/p") {
      output += "\n\n";
    } else if (tag == "br" || tag == "br/") {
      output.push_back('\n');
    } else if (tag == "pre") {
      output += "\n\n```\n";
    } else if (tag == "/pre") {
      output += "\n```\n\n";
    } else if (tag == "code" || tag == "/code") {
      output.push_back('`');
    }
    index = close + 1;
  }
  return decodeEntities(output);
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
  const size_t pendingRequired = context.pending.size() + length;
  if (pendingRequired > context.pending.capacity()) {
    const size_t capacity = std::max(pendingRequired, context.pending.capacity() * 2);
    if (capacity > kMaxFeedEntryBytes + 512 || !hasAllocationRoom(capacity + 1)) return false;
    context.pending.reserve(capacity);
  }
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
  lastDiagnostics_ = {};
  releaseArticleLayout();
  if (source_ == FeedSource::HackerNews) {
    loadHackerNewsTopics();
  } else {
    loadGeekNewsTopics();
  }
}

void GeekNewsActivity::loadGeekNewsTopics() {
  bool loaded = false;
  lastError_ = FeedLoadError::None;
  for (int attempt = 0; attempt < kNetworkAttempts; ++attempt) {
    topics_.clear();
    FeedStreamContext context;
    context.pending.reserve(2048);
    context.topics = &topics_;
    const FeedLoadError fetchError = fetchGeekNews(kFeedPath, &context, receiveFeedChunk, lastDiagnostics_);
    loaded = fetchError == FeedLoadError::None && !topics_.empty();
    if (loaded) break;
    lastError_ = fetchError == FeedLoadError::None ? FeedLoadError::Parse : fetchError;
    prepareNetworkRetry(lastError_);
    if (attempt + 1 < kNetworkAttempts) delay(kRetryDelayMs * static_cast<uint32_t>(attempt + 1));
  }
  if (!loaded) {
    topics_.clear();
    view_ = View::Error;
    requestUpdate();
    return;
  }
  lastError_ = FeedLoadError::None;
  selectedTopic_ = 0;
  view_ = View::Topics;
  requestUpdate();
}

void GeekNewsActivity::loadHackerNewsTopics() {
  bool loaded = false;
  lastError_ = FeedLoadError::None;
  Storage.mkdir("/.crosspoint");
  for (int attempt = 0; attempt < kNetworkAttempts && !loaded; ++attempt) {
    topics_.clear();
    Storage.remove(kHackerNewsTempPath);
    HalFile output = Storage.open(kHackerNewsTempPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!output) {
      lastError_ = FeedLoadError::StorageIo;
      break;
    }
    FileResponseContext context{&output, 0, kMaxHackerNewsFeedBytes};
    const FeedLoadError fetchError = fetchHttps(kHackerNewsHost, kHackerNewsFeedPath, "application/json", &context,
                                                receiveFileChunk, lastDiagnostics_);
    output.flush();
    const bool closed = output.close();
    if (fetchError != FeedLoadError::None || !closed || context.received == 0) {
      lastError_ = fetchError != FeedLoadError::None
                       ? fetchError
                       : (closed ? FeedLoadError::Response : FeedLoadError::StorageIo);
      Storage.remove(kHackerNewsTempPath);
      prepareNetworkRetry(lastError_);
      if (attempt + 1 < kNetworkAttempts) delay(kRetryDelayMs * static_cast<uint32_t>(attempt + 1));
      continue;
    }

    HalFile input = Storage.open(kHackerNewsTempPath, O_RDONLY);
    if (!input) {
      lastError_ = FeedLoadError::StorageIo;
      Storage.remove(kHackerNewsTempPath);
      break;
    }
    JsonDocument filter;
    filter["hits"][0]["objectID"] = true;
    filter["hits"][0]["title"] = true;
    filter["hits"][0]["url"] = true;
    filter["hits"][0]["author"] = true;
    filter["hits"][0]["points"] = true;
    filter["hits"][0]["num_comments"] = true;
    filter["hits"][0]["story_text"] = true;
    JsonDocument document;
    const DeserializationError error =
        deserializeJson(document, input, DeserializationOption::Filter(filter));
    input.close();
    Storage.remove(kHackerNewsTempPath);
    if (error) {
      lastError_ = error == DeserializationError::NoMemory ? FeedLoadError::Memory : FeedLoadError::Parse;
      if (attempt + 1 < kNetworkAttempts) delay(kRetryDelayMs * static_cast<uint32_t>(attempt + 1));
      continue;
    }

    const JsonArrayConst hits = document["hits"].as<JsonArrayConst>();
    topics_.reserve(kMaxTopics);
    for (const JsonObjectConst hit : hits) {
      if (topics_.size() >= kMaxTopics) break;
      Topic topic;
      topic.id = std::atoi(hit["objectID"] | "0");
      topic.title = hit["title"] | "";
      if (topic.id <= 0 || topic.title.empty()) continue;
      topic.url = hit["url"] | "";
      topic.text = hit["story_text"] | "";
      const char* author = hit["author"] | "";
      const int points = hit["points"] | 0;
      const int comments = hit["num_comments"] | 0;
      char subtitle[112];
      std::snprintf(subtitle, sizeof(subtitle), "%dp · %dc%s%s", points, comments, *author == '\0' ? "" : " · ",
                    author);
      topic.subtitle = subtitle;
      topics_.push_back(std::move(topic));
    }
    loaded = !topics_.empty();
    if (!loaded) lastError_ = FeedLoadError::Parse;
  }

  if (!loaded) {
    topics_.clear();
    view_ = View::Error;
    requestUpdate();
    return;
  }
  lastError_ = FeedLoadError::None;
  selectedTopic_ = 0;
  view_ = View::Topics;
  requestUpdate();
}

bool GeekNewsActivity::loadGeekNewsArticle(const int topicId) {
  std::string markdown;
  if (!fetchGeekNewsArticle(topicId, markdown, lastError_, lastDiagnostics_)) return false;
  layoutMarkdown(markdown);
  if (!layoutOverflow_ && articleLines_.empty()) lastError_ = FeedLoadError::Parse;
  return lastError_ == FeedLoadError::None;
}

bool GeekNewsActivity::loadHackerNewsArticle(const int topicId) {
  lastError_ = FeedLoadError::None;
  const auto found = std::find_if(topics_.begin(), topics_.end(), [topicId](const Topic& topic) {
    return topic.id == topicId;
  });
  if (found == topics_.end()) {
    lastError_ = FeedLoadError::Parse;
    return false;
  }

  const std::string discussionUrl =
      std::string("https://news.ycombinator.com/item?id=") + std::to_string(topicId);
  if (found->url.empty()) {
    std::string markdown = hackerNewsHtmlToMarkdown(found->text);
    if (trim(markdown).empty()) markdown = tr(STR_HACKERNEWS_ARTICLE_UNAVAILABLE);
    layoutMarkdown(markdown, discussionUrl);
    if (!layoutOverflow_ && articleLines_.empty()) lastError_ = FeedLoadError::Parse;
    return lastError_ == FeedLoadError::None;
  }

  if (!hackerNewsUrlCanBeArticle(found->url)) {
    layoutMarkdown(tr(STR_HACKERNEWS_ARTICLE_UNAVAILABLE), found->url);
    if (!layoutOverflow_ && articleLines_.empty()) lastError_ = FeedLoadError::Parse;
    return lastError_ == FeedLoadError::None;
  }

  Storage.mkdir("/.crosspoint");
  std::string markdown;
  bool fetched = false;
  for (int attempt = 0; attempt < kNetworkAttempts; ++attempt) {
    Storage.remove(kArticleTempPath);
    markdown.clear();
    HalFile output = Storage.open(kArticleTempPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!output) {
      lastError_ = FeedLoadError::StorageIo;
      break;
    }
    FileResponseContext context{&output, 0, kMaxArticleBytes};
    const std::string path = "/" + found->url;
    const FeedLoadError fetchError =
        fetchHttps(kExtractorHost, path, "text/markdown, text/plain;q=0.9", &context, receiveFileChunk,
                   lastDiagnostics_, kExtractorTimeoutMs);
    output.flush();
    const bool closed = output.close();
    const bool responseReady = fetchError == FeedLoadError::None && closed && context.received > 0;
    if (responseReady) {
      if (readArticleFile(markdown, lastError_, lastDiagnostics_)) {
        fetched = true;
        Storage.remove(kArticleTempPath);
        break;
      }
    } else {
      lastError_ = context.tooLarge
                       ? FeedLoadError::Memory
                       : (context.writeFailed || !closed
                              ? FeedLoadError::StorageIo
                              : (fetchError == FeedLoadError::None ? FeedLoadError::Response : fetchError));
    }
    Storage.remove(kArticleTempPath);
    if (responseReady || context.tooLarge || context.writeFailed) break;
    prepareNetworkRetry(lastError_);
    if (attempt + 1 < kNetworkAttempts) delay(kRetryDelayMs * static_cast<uint32_t>(attempt + 1));
  }
  if (!fetched) return false;
  lastError_ = FeedLoadError::None;

  splitExtractorResponse(markdown);
  if (trim(markdown).empty()) markdown = tr(STR_HACKERNEWS_ARTICLE_UNAVAILABLE);
  layoutMarkdown(markdown, found->url);
  if (!layoutOverflow_ && articleLines_.empty()) lastError_ = FeedLoadError::Parse;
  return lastError_ == FeedLoadError::None;
}

void GeekNewsActivity::loadArticle(const int topicId) {
  lastDiagnostics_ = {};
  releaseArticleLayout();
  articleScrapped_ = false;
  scrapStorageError_ = false;
  const bool loaded = source_ == FeedSource::HackerNews ? loadHackerNewsArticle(topicId)
                                                        : loadGeekNewsArticle(topicId);
  if (!loaded) {
    view_ = View::Error;
    requestUpdate();
    return;
  }
  releaseArticleWifi();
  currentPage_ = 0;
  if (source_ == FeedSource::GeekNews && scrapsLoaded_) {
    articleScrapped_ = scrapStore_.findIndex(pendingTopicId_, sourceUrl_) >= 0;
  }
  view_ = View::Article;
  requestUpdate();
}

bool GeekNewsActivity::ensureScrapsLoaded() {
  if (scrapsLoaded_) return !scrapStoreLoadFailed_;
  scrapsLoaded_ = true;
  scrapStoreLoadFailed_ = !scrapStore_.load();
  scrapStorageError_ = scrapStoreLoadFailed_;
  return !scrapStoreLoadFailed_;
}

void GeekNewsActivity::openScraps() {
  ensureScrapsLoaded();
  const size_t count = scrapStore_.getScraps().size();
  selectedScrap_ = count == 0 ? 0 : std::min(selectedScrap_, count - 1);
  view_ = View::Scraps;
  requestUpdate();
}

void GeekNewsActivity::openSelectedScrap() {
  const auto& scraps = scrapStore_.getScraps();
  if (selectedScrap_ >= scraps.size()) return;
  const GeekNewsScrap& scrap = scraps[selectedScrap_];
  pendingTopicId_ = scrap.topicId;
  articleTitle_ = scrap.title;
  articleBackView_ = View::Scraps;
  view_ = View::LoadingArticle;
  loadPending_ = true;
  requestUpdate();
}

void GeekNewsActivity::showSelectedScrapQr() {
  const auto& scraps = scrapStore_.getScraps();
  if (selectedScrap_ >= scraps.size()) return;
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, scraps[selectedScrap_].url),
                         [](const ActivityResult&) {});
}

void GeekNewsActivity::scrapArticle() {
  if (pendingTopicId_ <= 0 || articleTitle_.empty() || !ensureScrapsLoaded()) {
    scrapStorageError_ = true;
    requestUpdate();
    return;
  }

  const GeekNewsScrapStore::AddResult result =
      scrapStore_.add(GeekNewsScrap{pendingTopicId_, articleTitle_, GeekNewsScrapStore::topicUrl(pendingTopicId_)});
  articleScrapped_ = result != GeekNewsScrapStore::AddResult::Failed;
  scrapStorageError_ = result == GeekNewsScrapStore::AddResult::Failed;
  requestUpdate();
}

void GeekNewsActivity::deleteSelectedScrap() {
  const size_t count = scrapStore_.getScraps().size();
  if (selectedScrap_ >= count) return;
  scrapStorageError_ = !scrapStore_.removeAt(selectedScrap_);
  const size_t remaining = scrapStore_.getScraps().size();
  if (remaining == 0) {
    selectedScrap_ = 0;
  } else if (selectedScrap_ >= remaining) {
    selectedScrap_ = remaining - 1;
  }
}

const char* GeekNewsActivity::sourceName() const {
  return source_ == FeedSource::HackerNews ? tr(STR_HACKERNEWS) : tr(STR_GEEKNEWS);
}

const char* GeekNewsActivity::loadErrorMessage() const {
  switch (lastError_) {
    case FeedLoadError::Wifi:
      return tr(STR_FEED_ERROR_WIFI);
    case FeedLoadError::Dns:
      return tr(STR_FEED_ERROR_DNS);
    case FeedLoadError::Tls:
      return tr(STR_FEED_ERROR_TLS);
    case FeedLoadError::Http:
      return tr(STR_FEED_ERROR_HTTP);
    case FeedLoadError::Response:
      return tr(STR_FEED_ERROR_RESPONSE);
    case FeedLoadError::StorageIo:
      return tr(STR_FEED_ERROR_STORAGE);
    case FeedLoadError::Parse:
      return tr(STR_FEED_ERROR_PARSE);
    case FeedLoadError::Memory:
      return tr(STR_FEED_ERROR_MEMORY);
    case FeedLoadError::LayoutLimit:
      return tr(STR_FEED_ERROR_LAYOUT_LIMIT);
    case FeedLoadError::None:
    default:
      return "";
  }
}

void GeekNewsActivity::showArticleQr() {
  if (sourceUrl_.empty()) return;
  startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, sourceUrl_),
                         [](const ActivityResult&) {});
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
      prefix.empty() ? 0 : renderer.getTextAdvanceX(fontId, prefix.c_str(), EpdFontFamily::BOLD);

  RichLine line;
  line.indent = indent;
  line.height = lineHeight;
  line.quote = quote;
  std::vector<Span> lineSpans;
  lineSpans.reserve(std::min<size_t>(spans.size() + (prefix.empty() ? 0 : 1), 8));
  bool hasBodyText = false;
  if (!prefix.empty()) {
    lineSpans.push_back({prefix, InlineStyle::Bold});
  }

  auto styleFor = [](const InlineStyle style) {
    if (style == InlineStyle::Bold) return EpdFontFamily::BOLD;
    if (style == InlineStyle::Italic) return EpdFontFamily::ITALIC;
    return EpdFontFamily::REGULAR;
  };
  auto pushLine = [&]() -> bool {
    if (!storeLayoutLine(line, lineSpans)) return false;
    line = RichLine{};
    line.indent = indent + prefixWidth;
    line.height = lineHeight;
    line.quote = quote;
    lineSpans.clear();
    hasBodyText = false;
    return true;
  };
  auto measureLine = [&]() {
    int advance = 0;
    for (const Span& lineSpan : lineSpans) {
      advance += renderer.getTextAdvanceX(fontId, lineSpan.text.c_str(), styleFor(lineSpan.style));
    }
    return advance;
  };
  auto appendCharacter = [&](const std::string& character, const InlineStyle style) {
    if (!lineSpans.empty() && lineSpans.back().style == style) {
      lineSpans.back().text += character;
    } else {
      lineSpans.push_back({character, style});
    }
  };
  auto removeCharacter = [&](const size_t bytes) {
    Span& last = lineSpans.back();
    last.text.resize(last.text.size() - bytes);
    if (last.text.empty()) lineSpans.pop_back();
  };

  for (const Span& span : spans) {
    for (size_t index = 0; index < span.text.size();) {
      const size_t bytes = std::min(utf8CharBytes(static_cast<unsigned char>(span.text[index])),
                                    span.text.size() - index);
      const std::string character = span.text.substr(index, bytes);
      appendCharacter(character, span.style);
      int candidateWidth = measureLine();
      const int available = pageWidth - metrics.contentSidePadding * 2 - line.indent - (quote ? 10 : 0) -
                            kTextRightSafetyPx;
      if (hasBodyText && candidateWidth > available) {
        removeCharacter(bytes);
        if (!pushLine()) return;
        appendCharacter(character, span.style);
        candidateWidth = measureLine();
      }
      hasBodyText = true;
      index += bytes;
    }
  }
  if (!lineSpans.empty() && !pushLine()) return;
  if (spacingAfter > 0) {
    RichLine space;
    space.height = spacingAfter;
    storeLayoutLine(space);
  }
}

void GeekNewsActivity::appendMarkdownBlock(const std::string& text, const std::string& prefix, const int indent,
                                           const bool quote, const bool codeBlock, const int spacingAfter) {
  appendWrappedSpans(parseInline(text, codeBlock), prefix, indent, quote, spacingAfter);
}

void GeekNewsActivity::layoutMarkdown(const std::string& markdown, const std::string& explicitSourceUrl) {
  articleLines_.clear();
  articleSpans_.clear();
  articleText_.clear();
  pageStarts_.clear();
  sourceUrl_ = explicitSourceUrl;
  layoutOverflow_ = false;
  lastError_ = FeedLoadError::None;

  const size_t sourceMarker = markdown.find("- Original source:");
  if (sourceUrl_.empty() && sourceMarker != std::string::npos) {
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
    if (layoutOverflow_) break;
    const size_t end = body.find('\n', cursor);
    std::string line(body.substr(cursor, end == std::string_view::npos ? std::string_view::npos : end - cursor));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    cursor = end == std::string_view::npos ? body.size() + 1 : end + 1;

    if (trim(line).rfind("```", 0) == 0) {
      inCodeFence = !inCodeFence;
      if (!inCodeFence) {
        RichLine space;
        space.height = 8;
        storeLayoutLine(space);
      }
      continue;
    }
    if (inCodeFence) {
      appendMarkdownBlock(line.empty() ? " " : line, "", 12, false, true, 2);
      continue;
    }

    const std::string stripped = trim(line);
    if (stripped.empty()) {
      if (!articleLines_.empty() && articleLines_.back().spanCount > 0) {
        RichLine space;
        space.height = 8;
        storeLayoutLine(space);
      }
      continue;
    }
    if (stripped == "---" || stripped == "***" || stripped == "___") {
      RichLine rule;
      rule.height = 14;
      rule.rule = true;
      storeLayoutLine(rule);
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
  if (!layoutOverflow_) rebuildPageStarts();
}

bool GeekNewsActivity::storeLayoutLine(RichLine line, const std::vector<Span>& spans) {
  if (layoutOverflow_) return false;
  if (articleLines_.size() >= kMaxLayoutLines) {
    failLayout(FeedLoadDiagnostics::MemoryStage::LineLimit);
    return false;
  }
  if (spans.size() > kMaxLayoutSpans - articleSpans_.size()) {
    failLayout(FeedLoadDiagnostics::MemoryStage::SpanLimit);
    return false;
  }
  size_t textBytes = articleText_.size();
  for (const auto& span : spans) {
    if (span.text.size() >= kMaxLayoutTextBytes - textBytes) {
      failLayout(FeedLoadDiagnostics::MemoryStage::TextLimit);
      return false;
    }
    textBytes += span.text.size() + 1;
  }
  // 작은 단위로 확장하되 재할당 전에 연속 빈 공간과 안전 여유를 검사한다.
  if (!ensureLayoutCapacity(articleLines_, articleLines_.size() + 1, kMaxLayoutLines, 128,
                            FeedLoadDiagnostics::MemoryStage::LineBuffer) ||
      !ensureLayoutCapacity(articleSpans_, articleSpans_.size() + spans.size(), kMaxLayoutSpans, 256,
                            FeedLoadDiagnostics::MemoryStage::SpanBuffer) ||
      !ensureLayoutCapacity(articleText_, textBytes, kMaxLayoutTextBytes, 4096,
                            FeedLoadDiagnostics::MemoryStage::TextBuffer)) return false;
  line.spanStart = static_cast<uint16_t>(articleSpans_.size());
  line.spanCount = static_cast<uint16_t>(spans.size());
  for (const auto& span : spans) {
    articleSpans_.push_back({static_cast<uint16_t>(articleText_.size()), span.style});
    articleText_.insert(articleText_.end(), span.text.begin(), span.text.end());
    articleText_.push_back('\0');
  }
  articleLines_.push_back(line);
  return true;
}

void GeekNewsActivity::rebuildPageStarts() {
  pageStarts_.clear();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int availableHeight = renderer.getScreenHeight() - metrics.topPadding - metrics.headerHeight -
                              metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int used = 0;
  for (size_t index = 0; index < articleLines_.size(); ++index) {
    const int height = articleLines_[index].height;
    if (index == 0 || (used > 0 && used + height > availableHeight)) {
      if (!ensureLayoutCapacity(pageStarts_, pageStarts_.size() + 1, kMaxLayoutLines, 32,
                                FeedLoadDiagnostics::MemoryStage::PageBuffer)) return;
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
  if (view_ == View::Sources) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      pendingTopicId_ = 0;
      topics_.clear();
      connectWifi();
      return;
    }
    buttonNavigator_.onNextRelease([this] {
      source_ = source_ == FeedSource::GeekNews ? FeedSource::HackerNews : FeedSource::GeekNews;
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this] {
      source_ = source_ == FeedSource::GeekNews ? FeedSource::HackerNews : FeedSource::GeekNews;
      requestUpdate();
    });
    return;
  }
  if (view_ == View::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::Sources;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      retryLoad();
    }
    return;
  }
  if (view_ == View::Topics) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::Sources;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && selectedTopic_ < topics_.size()) {
      pendingTopicId_ = topics_[selectedTopic_].id;
      articleTitle_ = topics_[selectedTopic_].title;
      articleBackView_ = View::Topics;
      view_ = View::LoadingArticle;
      loadPending_ = true;
      requestUpdate();
      return;
    }
    if (source_ == FeedSource::GeekNews && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      openScraps();
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

  if (view_ == View::Scraps) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::Topics;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
        selectedScrap_ < scrapStore_.getScraps().size()) {
      view_ = View::ConfirmDelete;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      openSelectedScrap();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      showSelectedScrapQr();
      return;
    }
    const int count = static_cast<int>(scrapStore_.getScraps().size());
    if (count == 0) return;
    const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true,
                                                                         scrapStorageError_ ? 28 : 0);
    buttonNavigator_.onRelease({MappedInputManager::Button::Down}, [this, count] {
      selectedScrap_ = ButtonNavigator::nextIndex(static_cast<int>(selectedScrap_), count);
      requestUpdate();
    });
    buttonNavigator_.onRelease({MappedInputManager::Button::Up}, [this, count] {
      selectedScrap_ = ButtonNavigator::previousIndex(static_cast<int>(selectedScrap_), count);
      requestUpdate();
    });
    buttonNavigator_.onContinuous({MappedInputManager::Button::Down}, [this, count, pageItems] {
      selectedScrap_ = ButtonNavigator::nextPageIndex(static_cast<int>(selectedScrap_), count, pageItems);
      requestUpdate();
    });
    buttonNavigator_.onContinuous({MappedInputManager::Button::Up}, [this, count, pageItems] {
      selectedScrap_ = ButtonNavigator::previousPageIndex(static_cast<int>(selectedScrap_), count, pageItems);
      requestUpdate();
    });
    return;
  }

  if (view_ == View::ConfirmDelete) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      deleteSelectedScrap();
      view_ = View::Scraps;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      view_ = View::Scraps;
      requestUpdate();
    }
    return;
  }

  const bool previousPage = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextPage = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    view_ = articleBackView_;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (source_ == FeedSource::GeekNews) {
      scrapArticle();
    } else {
      showArticleQr();
    }
  } else if (previousPage && currentPage_ > 0) {
    --currentPage_;
    requestUpdate();
  } else if (nextPage && currentPage_ + 1 < pageStarts_.size()) {
    ++currentPage_;
    requestUpdate();
  }
}

void GeekNewsActivity::drawStatus(const char* message, const bool retry) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 sourceName());
  const int centerY = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, centerY - (retry ? 14 : 0), message, true, EpdFontFamily::BOLD);
  if (retry && lastError_ != FeedLoadError::None) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 20, loadErrorMessage());
    if (lastDiagnostics_.stage != FeedLoadDiagnostics::Stage::None) {
      // 직렬 연결 없이도 마지막 시도의 단계·오류 코드·수신량을 사진으로 전달할 수 있다.
      char details[96];
      std::snprintf(details, sizeof(details), "%s %d | %u/%d B | %us%s%s",
                    lastDiagnostics_.stage == FeedLoadDiagnostics::Stage::Headers ? "H" : "B",
                    lastDiagnostics_.code, static_cast<unsigned>(lastDiagnostics_.received),
                    lastDiagnostics_.expected, static_cast<unsigned>(lastDiagnostics_.elapsedMs / 1000),
                    lastDiagnostics_.sinkFailed ? " S" : "", lastDiagnostics_.timedOut ? " T" : "");
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + 50, details);
    }
    if (lastDiagnostics_.memoryStage != FeedLoadDiagnostics::MemoryStage::None) {
      const char* stage = "";
      switch (lastDiagnostics_.memoryStage) {
        case FeedLoadDiagnostics::MemoryStage::ArticleBuffer: stage = "READ"; break;
        case FeedLoadDiagnostics::MemoryStage::LineBuffer: stage = "LINES"; break;
        case FeedLoadDiagnostics::MemoryStage::SpanBuffer: stage = "SPANS"; break;
        case FeedLoadDiagnostics::MemoryStage::TextBuffer: stage = "TEXT"; break;
        case FeedLoadDiagnostics::MemoryStage::PageBuffer: stage = "PAGES"; break;
        case FeedLoadDiagnostics::MemoryStage::LineLimit: stage = "LINE-LIMIT"; break;
        case FeedLoadDiagnostics::MemoryStage::SpanLimit: stage = "SPAN-LIMIT"; break;
        case FeedLoadDiagnostics::MemoryStage::TextLimit: stage = "TEXT-LIMIT"; break;
        default: break;
      }
      char details[96];
      std::snprintf(details, sizeof(details), "M %s | need=%u B", stage,
                    static_cast<unsigned>(lastDiagnostics_.requestedBytes));
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + 80, details);
      std::snprintf(details, sizeof(details), "heap=%u B | max=%u B", static_cast<unsigned>(lastDiagnostics_.freeHeap),
                    static_cast<unsigned>(lastDiagnostics_.largestBlock));
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + 110, details);
    }
  }
  const auto labels = mappedInput.mapLabels(retry ? tr(STR_BACK) : "", retry ? tr(STR_RETRY) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawSources() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const size_t selected = source_ == FeedSource::GeekNews ? 0 : 1;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FEEDS));
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, 2, selected,
               [](const int index) { return std::string(index == 0 ? tr(STR_GEEKNEWS) : tr(STR_HACKERNEWS)); },
               [](const int) { return std::string(); }, nullptr, nullptr);
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawTopics() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, sourceName());
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, topics_.size(), selectedTopic_,
               [this](const int index) { return topics_[index].title; },
               [this](const int index) { return topics_[index].subtitle; }, nullptr, nullptr);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN),
                                             source_ == FeedSource::GeekNews ? tr(STR_GEEKNEWS_SCRAPS) : "",
                                             tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawArticle() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int fontId = SETTINGS.getReaderFontId();
  char pageLabel[24];
  std::snprintf(pageLabel, sizeof(pageLabel), "%zu/%zu", currentPage_ + 1, pageStarts_.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 sourceName(), pageLabel);

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
    if (line.spanCount == 0) {
      y += line.height;
      continue;
    }
    int x = metrics.contentSidePadding + line.indent + (line.quote ? 10 : 0);
    if (line.quote) renderer.drawLine(x - 8, y, x - 8, y + line.height - 2, 2, true);
    const size_t spanEnd = line.spanStart + line.spanCount;
    for (size_t spanIndex = line.spanStart; spanIndex < spanEnd; ++spanIndex) {
      const StoredSpan& span = articleSpans_[spanIndex];
      const char* text = articleText_.data() + span.textOffset;
      EpdFontFamily::Style style = EpdFontFamily::REGULAR;
      if (span.style == InlineStyle::Bold) style = EpdFontFamily::BOLD;
      if (span.style == InlineStyle::Italic) style = EpdFontFamily::ITALIC;
      const int width = renderer.getTextAdvanceX(fontId, text, style);
      if (span.style == InlineStyle::Code) renderer.drawRect(x - 1, y - 1, width + 2, line.height - 2);
      renderer.drawText(fontId, x, y, text, true, style);
      if (span.style == InlineStyle::Link) renderer.drawLine(x, y + line.height - 4, x + width, y + line.height - 4);
      x += width;
    }
    y += line.height;
  }
  const char* actionLabel = "QR";
  if (source_ == FeedSource::GeekNews) {
    actionLabel = scrapStorageError_ ? tr(STR_GEEKNEWS_SCRAP_FAILED)
                                     : (articleScrapped_ ? tr(STR_GEEKNEWS_SCRAPPED) : tr(STR_GEEKNEWS_SCRAP));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), actionLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawScraps() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GEEKNEWS_SCRAPS));

  if (scrapStorageError_) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop, tr(STR_GEEKNEWS_SCRAP_STORAGE_ERROR));
    contentTop += 28;
    contentHeight -= 28;
  }

  const auto& scraps = scrapStore_.getScraps();
  if (scraps.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_GEEKNEWS_NO_SCRAPS), true,
                              EpdFontFamily::BOLD);
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, scraps.size(), selectedScrap_,
                 [&scraps](const int index) { return scraps[index].title; },
                 [&scraps](const int index) { return scraps[index].url; }, nullptr, nullptr);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), scraps.empty() ? "" : tr(STR_DELETE),
                                             scraps.empty() ? "" : tr(STR_OPEN), scraps.empty() ? "" : "QR");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::drawDeleteConfirmation() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GEEKNEWS_SCRAPS));
  renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2 - 15,
                            tr(STR_GEEKNEWS_DELETE_SCRAP_CONFIRM), true, EpdFontFamily::BOLD);
  const auto labels = mappedInput.mapLabels("", "", tr(STR_NO), tr(STR_YES));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void GeekNewsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (view_ == View::Sources) {
    drawSources();
  } else if (view_ == View::LoadingTopics || view_ == View::LoadingArticle) {
    drawStatus(tr(STR_LOADING), false);
  } else if (view_ == View::Error) {
    drawStatus(source_ == FeedSource::HackerNews ? tr(STR_HACKERNEWS_LOAD_FAILED) : tr(STR_GEEKNEWS_LOAD_FAILED),
               true);
  } else if (view_ == View::Topics) {
    drawTopics();
  } else if (view_ == View::Scraps) {
    drawScraps();
  } else if (view_ == View::ConfirmDelete) {
    drawDeleteConfirmation();
  } else {
    drawArticle();
  }
  renderer.displayBuffer();
}
