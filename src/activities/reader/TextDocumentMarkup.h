#pragma once

#include <algorithm>
#include <string>
#include <string_view>

// 파일 입출력과 분리한 제한적 마크다운 → XHTML 변환기.
// 생성한 태그만 허용하므로 원문의 HTML, 이미지, 링크가 외부 파일을 읽게 하지 않는다.
namespace TextDocumentMarkup {
constexpr size_t MAX_LINE_BYTES = 4096;

constexpr std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

template <typename Sink>
constexpr bool escape(std::string_view text, Sink& sink, bool preserveSpaces = false, bool entities = false) {
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char ch = text[i];
    if (entities && ch == '&') {
      const auto end = text.find(';', i + 1);
      if (end != std::string_view::npos && end - i <= 12) {
        const auto entity = text.substr(i, end - i + 1);
        if (entity == "&amp;" || entity == "&lt;" || entity == "&gt;" || entity == "&quot;" || entity == "&apos;") {
          if (!sink(entity)) return false;
          i = end;
          continue;
        }
        if (entity == "&nbsp;") {
          if (!sink("&#160;")) return false;
          i = end;
          continue;
        }
      }
    }
    if (ch == '&') { if (!sink("&amp;")) return false; }
    else if (ch == '<') { if (!sink("&lt;")) return false; }
    else if (ch == '>') { if (!sink("&gt;")) return false; }
    else if (ch == '\t') { if (!sink(preserveSpaces ? "&#160;&#160;&#160;&#160;" : " ")) return false; }
    else if (ch == ' ' && preserveSpaces) { if (!sink("&#160;")) return false; }
    else if (ch < 32) { if (!sink(" ")) return false; }
    else if (!sink(text.substr(i, 1))) return false;
  }
  return true;
}

template <typename Sink>
constexpr bool inlineText(std::string_view text, Sink& sink, unsigned depth = 0) {
  if (depth >= 8) return escape(text, sink);
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      // 이스케이프는 ASCII 문장 부호에만 적용한다.
      const char next = text[i + 1];
      if (std::string_view("\\`*_{}[]()#+-.!>|~").find(next) != std::string_view::npos) {
        if (!escape(text.substr(i + 1, 1), sink)) return false;
        i += 2;
        continue;
      }
    }
    const bool image = text.substr(i, 2) == "![";
    if (image || text[i] == '[') {
      const size_t start = i + (image ? 2 : 1);
      const size_t labelEnd = text.find("](", start);
      if (labelEnd != std::string_view::npos) {
        size_t end = labelEnd + 2;
        int nesting = 1;
        for (; end < text.size(); ++end) {
          if (text[end] == '\\' && end + 1 < text.size()) { ++end; continue; }
          if (text[end] == '(') ++nesting;
          if (text[end] == ')' && --nesting == 0) break;
        }
        if (end < text.size()) {
          if (!sink(image ? "[" : "<u>") || !inlineText(text.substr(start, labelEnd - start), sink, depth + 1) ||
              !sink(image ? "]" : "</u>")) return false;
          i = end + 1;
          continue;
        }
      }
    }
    if (text[i] == '*' || text[i] == '_' || text[i] == '`') {
      const char marker = text[i];
      size_t count = 1;
      while (i + count < text.size() && text[i + count] == marker && count < 3) ++count;
      const bool intraword = marker == '_' && i > 0 && text[i - 1] != ' ';
      const auto delimiter = text.substr(i, count);
      const size_t end = text.find(delimiter, i + count);
      if (!intraword && end != std::string_view::npos && end > i + count) {
        const auto body = text.substr(i + count, end - i - count);
        const auto open = marker == '`' ? "<u>" : count == 3 ? "<b><i>" : count == 2 ? "<b>" : "<i>";
        const auto close = marker == '`' ? "</u>" : count == 3 ? "</i></b>" : count == 2 ? "</b>" : "</i>";
        if (!sink(open) || !(marker == '`' ? escape(body, sink, true) : inlineText(body, sink, depth + 1)) ||
            !sink(close)) return false;
        i = end + count;
        continue;
      }
    }
    if (text[i] == '&') {
      const auto end = text.find(';', i + 1);
      if (end != std::string_view::npos && end - i <= 12) {
        if (!escape(text.substr(i, end - i + 1), sink, false, true)) return false;
        i = end + 1;
        continue;
      }
    }
    if (!escape(text.substr(i, 1), sink)) return false;
    ++i;
  }
  return true;
}

template <typename Sink>
class Converter {
  Sink& sink;
  bool markdown;
  std::string line;
  bool firstLine = true;
  bool afterCR = false;
  bool longLine = false;
  char fence = 0;
  size_t fenceLength = 0;

  constexpr bool emitLine() {
    if (firstLine) {
      if (line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
      firstLine = false;
    }
    if (longLine) {
      const bool ok = escape(line, sink, fence != 0) && sink("</p>\n");
      line.clear();
      longLine = false;
      return ok;
    }
    const bool ok = block(line);
    line.clear();
    return ok;
  }

  constexpr bool block(std::string_view source) {
    auto content = trim(source);
    if (!markdown) {
      return sink(content.empty() ? "<p style=\"text-indent:0\">&#160;" : "<p>") &&
             escape(source, sink) && sink("</p>\n");
    }
    size_t run = 0;
    if (!content.empty() && (content.front() == '`' || content.front() == '~')) {
      while (run < content.size() && content[run] == content.front()) ++run;
    }
    if (run >= 3 && (!fence || (content.front() == fence && run >= fenceLength && trim(content.substr(run)).empty()))) {
      if (fence) { fence = 0; fenceLength = 0; }
      else { fence = content.front(); fenceLength = run; }
      return true;
    }
    if (fence) {
      return sink("<p style=\"text-indent:0;text-align:left;margin-left:4%\">") &&
             (source.empty() ? sink("&#160;") : escape(source, sink, true)) && sink("</p>\n");
    }
    // 빈 줄은 문단 경계로 처리하고 여백은 독서 설정에 맡긴다.
    if (content.empty()) return true;
    if (content == "---" || content == "***" || content == "___") {
      return sink("<p style=\"text-indent:0;text-align:center\">────────</p>\n");
    }
    size_t heading = 0;
    while (heading < content.size() && content[heading] == '#') ++heading;
    if (heading > 0 && heading <= 6 && heading < content.size() && content[heading] == ' ') {
      return sink("<p style=\"text-indent:0;margin-top:0.5em;margin-bottom:0.3em\"><b>") &&
             inlineText(trim(content.substr(heading + 1)), sink) && sink("</b></p>\n");
    }
    size_t leading = 0;
    for (char ch : source) {
      if (ch == ' ') ++leading;
      else if (ch == '\t') leading += 4;
      else break;
    }
    size_t number = 0;
    while (number < content.size() && content[number] >= '0' && content[number] <= '9') ++number;
    const bool ordered = number > 0 && number < 10 && content.size() > number + 1 &&
                         content[number] == '.' && content[number + 1] == ' ';
    const bool bullet = content.size() > 1 && (content[0] == '-' || content[0] == '*' || content[0] == '+') && content[1] == ' ';
    const bool quote = content.front() == '>';
    const size_t indent = std::min<size_t>(leading / 2, 5);
    constexpr std::string_view margins[] = {"6%", "10%", "14%", "18%", "22%", "26%"};
    if (ordered || bullet || quote) {
      if (!sink("<p style=\"text-indent:-1em;margin-left:") || !sink(margins[indent]) || !sink("\">")) return false;
      if (ordered) {
        if (!escape(content.substr(0, number + 2), sink)) return false;
        content.remove_prefix(number + 2);
      } else {
        if (!sink(quote ? "│ " : "• ")) return false;
        content = trim(content.substr(quote ? 1 : 2));
      }
    } else if (!sink("<p>")) return false;
    // 표는 피드와 같이 셀 사이를 구분한 텍스트로 표시한다.
    if (content.size() >= 2 && content.front() == '|' && content.back() == '|') {
      content.remove_prefix(1);
      content.remove_suffix(1);
    }
    return inlineText(content, sink) && sink("</p>\n");
  }

 public:
  constexpr Converter(Sink& sink, bool markdown) : sink(sink), markdown(markdown) {}
  constexpr bool begin() { return sink("<html><body>"); }
  constexpr bool push(std::string_view chunk) {
    for (char ch : chunk) {
      if (ch == '\n' && afterCR) { afterCR = false; continue; }
      afterCR = ch == '\r';
      if (ch == '\r' || ch == '\n') {
        if (!emitLine()) return false;
        continue;
      }
      if (line.size() == MAX_LINE_BYTES) {
        // 비정상적으로 긴 물리 행은 서식 해석을 생략하고 내용은 끝까지 보존한다.
        if (!longLine) {
          if (firstLine && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
          firstLine = false;
          if (!sink(fence ? "<p style=\"text-indent:0;margin-left:4%\">" : "<p>")) return false;
          longLine = true;
        }
        if (!escape(line, sink, fence != 0)) return false;
        line.clear();
      }
      line.push_back(ch);
    }
    return true;
  }
  constexpr bool finish() { return ((line.empty() && !longLine) || emitLine()) && sink("</body></html>"); }
};
}  // namespace TextDocumentMarkup
