#include "activities/reader/TextDocumentMarkup.h"
#include "activities/reader/TextReaderCache.h"

#include <array>

namespace {
constexpr std::string convert(std::string_view input, bool markdown, size_t chunk = 1024) {
  std::string output;
  auto sink = [&](std::string_view text) { output.append(text); return true; };
  TextDocumentMarkup::Converter converter(sink, markdown);
  if (!converter.begin()) return "ERROR";
  for (size_t i = 0; i < input.size(); i += chunk) {
    if (!converter.push(input.substr(i, chunk))) return "ERROR";
  }
  if (!converter.finish()) return "ERROR";
  return output;
}

constexpr bool plainText() {
  return convert("\xef\xbb\xbf" "첫 문단 <예시> & 글\r\n\r\n次の段落\r끝", false, 1) ==
         "<html><body><p>첫 문단 &lt;예시&gt; &amp; 글</p>\n"
         "<p style=\"text-indent:0\">&#160;</p>\n<p>次の段落</p>\n<p>끝</p>\n</body></html>";
}
static_assert(plainText());
static_assert(convert("# 제목 **그대로**", false).find("# 제목 **그대로**") != std::string::npos);
static_assert(convert("", true) == "<html><body></body></html>");

constexpr bool inlineStyles() {
  const auto html = convert("앞**강조**뒤 *기울임* __굵게__ ***겹침*** `a < b` [안내](https://example.org/a(b)) ![그림](image.png)", true, 1);
  return html.find("앞<b>강조</b>뒤") != std::string::npos &&
         html.find("<i>기울임</i>") != std::string::npos && html.find("<b>굵게</b>") != std::string::npos &&
         html.find("<b><i>겹침</i></b>") != std::string::npos &&
         html.find("<u>a&#160;&lt;&#160;b</u>") != std::string::npos &&
         html.find("<u>안내</u> [그림]") != std::string::npos && html.find("https://") == std::string::npos;
}
static_assert(inlineStyles());

constexpr bool blocks() {
  const auto html = convert("## 작은 제목\n- 첫 항목\n  - 안쪽 항목\n2. 둘째 항목\n> 인용문\n---\n| 가 | 나 |\n", true);
  return html.find("<b>작은 제목</b>") != std::string::npos &&
         html.find("margin-left:6%\">• 첫 항목") != std::string::npos &&
         html.find("margin-left:10%\">• 안쪽 항목") != std::string::npos &&
         html.find("2. 둘째 항목") != std::string::npos && html.find("│ 인용문") != std::string::npos &&
         html.find("────────") != std::string::npos && html.find(" 가 | 나 ") != std::string::npos;
}
static_assert(blocks());

constexpr bool fences() {
  const auto html = convert("````cpp\n  **literal**\n```\n~~~~\n````\n**bold**\n~~~\nlast <line>", true, 1);
  return html.find("&#160;&#160;**literal**") != std::string::npos &&
         html.find("```</p>") != std::string::npos && html.find("~~~~</p>") != std::string::npos &&
         html.find("<b>bold</b>") != std::string::npos && html.find("last&#160;&lt;line&gt;") != std::string::npos;
}
static_assert(fences());

constexpr bool escaping() {
  const auto html = convert("\\*literal\\* snake_case &amp; &nbsp; &unknown; <img src='local'/> **unclosed", true);
  return html.find("*literal* snake_case &amp; &#160; &amp;unknown;") != std::string::npos &&
         html.find("&lt;img src='local'/&gt;") != std::string::npos && html.find("**unclosed") != std::string::npos;
}
static_assert(escaping());

constexpr bool longLine() {
  std::string input;
  for (int i = 0; i < 1700; ++i) input += "あ";
  input += "<&END\n**next**";
  const auto html = convert(input, true, 7);
  const auto expected = std::string("<html><body><p>") + input.substr(0, 5100) +
                        "&lt;&amp;END</p>\n<p><b>next</b></p>\n</body></html>";
  return html == expected;
}
static_assert(longLine());

constexpr bool sinkFailure() {
  size_t written = 0;
  auto sink = [&](std::string_view text) { written += text.size(); return written < 50; };
  TextDocumentMarkup::Converter converter(sink, true);
  return converter.begin() && !converter.push("# synthetic heading\n");
}
static_assert(sinkFailure());

constexpr bool cacheHeaders() {
  std::array<uint8_t, 64> data{};
  auto put = [&](size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data[offset + i] = (value >> (i * 8)) & 255;
  };
  put(0, TextReaderCache::MAGIC);
  data[4] = 4;
  put(5, 800);
  put(31, 12);
  if (TextReaderCache::pageCount(data.data(), 35, 800) != 12) return false;
  data = {};
  put(0, TextReaderCache::MAGIC);
  put(4, TextReaderCache::VERSION);
  put(8, 800);
  put(60, 18);
  if (TextReaderCache::pageCount(data.data(), 64, 800) != 18 ||
      TextReaderCache::pageCount(data.data(), 63, 800) != 0 ||
      TextReaderCache::pageCount(data.data(), 64, 799) != 0) return false;
  put(60, 65536);
  return TextReaderCache::pageCount(data.data(), 64, 800) == 0;
}
static_assert(cacheHeaders());
}  // namespace
