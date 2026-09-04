#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 텍스트 전용 PDF 해석기. 파일과 압축 입출력은 Backend로 분리하여 실제 코드의 호스트 검증을 허용한다.
namespace PdfText {
enum class Error { None, Io, Invalid, Unsupported, Encrypted, Encoding, NoText, Limit, Memory };
enum class Kind { End, Word, Name, String, Array, Dict, Ref };
struct Ref {
  uint32_t id = 0, generation = 0;
  constexpr bool operator==(const Ref& other) const { return id == other.id && generation == other.generation; }
};
struct Value {
  Kind kind = Kind::End;
  std::string text;
  Ref ref;
  std::vector<Value> items;
  constexpr const Value* get(std::string_view key) const {
    if (kind != Kind::Dict) return nullptr;
    for (size_t i = 0; i + 1 < items.size(); i += 2) if (items[i].text == key) return &items[i + 1];
    return nullptr;
  }
  constexpr bool is(std::string_view name) const { return kind == Kind::Name && text == name; }
};
constexpr bool whitespace(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
constexpr bool delimiter(int c) {
  return c < 0 || whitespace(c) || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '/' || c == '%';
}
constexpr int hex(int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
         c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
constexpr bool integer(std::string_view text, uint32_t& value) {
  value = 0;
  if (text.empty()) return false;
  for (char ch : text) {
    if (ch < '0' || ch > '9' || value > (UINT32_MAX - (ch - '0')) / 10) return false;
    value = value * 10 + (ch - '0');
  }
  return true;
}
constexpr bool number(std::string_view text, double& value) {
  value = 0;
  bool negative = false, decimal = false, digit = false;
  double fraction = 0.1;
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
    negative = text.front() == '-'; text.remove_prefix(1);
  }
  for (char ch : text) {
    if (ch == '.' && !decimal) { decimal = true; continue; }
    if (ch < '0' || ch > '9') return false;
    digit = true;
    if (decimal) { value += (ch - '0') * fraction; fraction *= 0.1; }
    else value = value * 10 + ch - '0';
    if (value > 1e9) return false;
  }
  if (negative) value = -value;
  return digit;
}

template <typename Backend>
class Lexer {
  Backend& io;
  unsigned source;
  uint32_t pos, end;
 public:
  constexpr Lexer(Backend& io, unsigned source, uint32_t pos, uint32_t end)
      : io(io), source(source), pos(pos), end(end) {}
  constexpr uint32_t position() const { return pos; }
  constexpr void seek(uint32_t offset) { pos = offset; }
  constexpr int peek() { return pos < end ? io.byte(source, pos) : -1; }
  constexpr int take() { const int ch = peek(); if (ch >= 0) ++pos; return ch; }
  constexpr bool fail(Error error) { if (io.error == Error::None) io.error = error; return false; }
  constexpr void skip() {
    while (io.error == Error::None) {
      if (whitespace(peek())) { take(); continue; }
      if (peek() != '%') break;
      while (peek() >= 0 && peek() != 10 && peek() != 13) take();
    }
  }
  constexpr bool append(std::string& text, int ch) {
    if (text.size() >= 4096) return fail(Error::Limit);
    if (text.size() == text.capacity() && !io.room(text.size() * 2 + 64)) return fail(Error::Memory);
    text.push_back(static_cast<char>(ch));
    return true;
  }
  constexpr Value token() {
    Value result;
    skip();
    int ch = take();
    if (ch < 0) return result;
    result.kind = Kind::Word;
    if (ch == '/') {
      result.kind = Kind::Name;
      while (!delimiter(peek())) {
        ch = take();
        if (ch == '#') {
          const int a = hex(take()), b = hex(take());
          if (a < 0 || b < 0) { fail(Error::Invalid); return {}; }
          ch = a * 16 + b;
        }
        if (!append(result.text, ch)) return {};
      }
    } else if (ch == '(') {
      result.kind = Kind::String;
      int depth = 1;
      while (depth && io.error == Error::None) {
        ch = take();
        if (ch < 0) { fail(Error::Invalid); return {}; }
        if (ch == '\\') {
          ch = take();
          if (ch < 0) { fail(Error::Invalid); return {}; }
          if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && peek() == '\n') take();
            continue;
          }
          if (ch >= '0' && ch <= '7') {
            int value = ch - '0';
            for (int n = 1; n < 3 && peek() >= '0' && peek() <= '7'; ++n) value = value * 8 + take() - '0';
            ch = value & 255;
          } else if (ch == 'n') ch = '\n';
          else if (ch == 'r') ch = '\r';
          else if (ch == 't') ch = '\t';
          else if (ch == 'b') ch = '\b';
          else if (ch == 'f') ch = '\f';
        } else if (ch == '(') { if (++depth > 32) { fail(Error::Limit); return {}; } }
        else if (ch == ')' && --depth == 0) break;
        if (!append(result.text, ch)) return {};
      }
    } else if (ch == '<' && peek() != '<') {
      result.kind = Kind::String;
      int high = -1;
      while (io.error == Error::None) {
        ch = take();
        if (ch == '>') break;
        if (whitespace(ch)) continue;
        const int digit = hex(ch);
        if (digit < 0) { fail(Error::Invalid); return {}; }
        if (high < 0) high = digit;
        else { if (!append(result.text, high * 16 + digit)) return {}; high = -1; }
      }
      if (high >= 0 && !append(result.text, high * 16)) return {};
    } else {
      if (!append(result.text, ch)) return {};
      if ((ch == '<' && peek() == '<') || (ch == '>' && peek() == '>')) append(result.text, take());
      else if (ch != '[' && ch != ']' && ch != ')' && ch != '>') {
        while (!delimiter(peek()) && io.error == Error::None) if (!append(result.text, take())) return {};
      }
    }
    return result;
  }
  constexpr Value value(unsigned depth = 0, unsigned* count = nullptr) {
    unsigned local = 0;
    if (!count) count = &local;
    if (depth > 8 || ++*count > 1024) { fail(Error::Limit); return {}; }
    if (!io.room(256)) { fail(Error::Memory); return {}; }
    Value result = token();
    if (result.kind != Kind::Word) return result;
    if (result.text == "[" || result.text == "<<") {
      const bool dict = result.text == "<<";
      result.kind = dict ? Kind::Dict : Kind::Array;
      result.text.clear();
      while (io.error == Error::None) {
        const uint32_t start = position();
        const Value next = token();
        if (next.kind == Kind::End) { fail(Error::Invalid); return {}; }
        if (next.kind == Kind::Word && next.text == (dict ? ">>" : "]")) break;
        seek(start);
        if (result.items.size() == result.items.capacity()) {
          const size_t capacity = result.items.size() + 8;
          if (!io.room(capacity * sizeof(Value))) { fail(Error::Memory); return {}; }
          result.items.reserve(capacity);
        }
        result.items.push_back(value(depth + 1, count));
        if (dict && result.items.size() % 2 == 1 && result.items.back().kind != Kind::Name) {
          fail(Error::Invalid); return {};
        }
      }
      if (dict && result.items.size() % 2) { fail(Error::Invalid); return {}; }
    } else {
      uint32_t id = 0, gen = 0;
      if (integer(result.text, id)) {
        const uint32_t after = position();
        const Value second = token();
        if (second.kind == Kind::Word && integer(second.text, gen)) {
          const Value third = token();
          if (third.kind == Kind::Word && third.text == "R") {
            result.kind = Kind::Ref; result.ref = {id, gen}; result.text.clear(); return result;
          }
        }
        seek(after);
      }
    }
    return result;
  }
};

struct Object { Value value; uint32_t stream = 0; };
struct Xref { uint32_t first, count, position; };
struct Mapping { uint64_t target; uint32_t first, last; uint8_t bytes, targetBytes; };
struct CodeSpace { uint32_t first, last; uint8_t bytes; };

template <typename Backend, typename Sink>
class Parser {
  Backend& io;
  Sink& sink;
  std::vector<Xref> sections;
  std::vector<Mapping> mappings;
  std::vector<CodeSpace> spaces;
  Ref root, fontRef;
  bool mapped = false, winAnsi = false;
  unsigned pageCount = 0, visited = 0;
  uint32_t emitted = 0;
  uint32_t characters = 0;
  char last = '\n';

  constexpr bool fail(Error e) { if (io.error == Error::None) io.error = e; return false; }
  constexpr bool write(std::string_view text) {
    if (text.size() > 16 * 1024 * 1024 - emitted) return fail(Error::Limit);
    if (!sink(text)) return fail(Error::Io);
    emitted += text.size();
    if (!text.empty()) last = text.back();
    return true;
  }
  constexpr bool newline() { return last == '\n' || write("\n"); }
  constexpr bool space() { return last == '\n' || last == ' ' || write(" "); }
  constexpr bool emitCodepoint(uint32_t code) {
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return fail(Error::Encoding);
    if (code == 0) return fail(Error::Encoding);
    if (code == 0xfeff) return true;
    if (code == '\n' || code == '\r') return newline();
    if (code < 32 || code == 160) return space();
    if (code > 32) ++characters;
    char bytes[4]{}; size_t size = 0;
    if (code < 128) bytes[size++] = char(code);
    else if (code < 2048) { bytes[size++] = char(0xc0 | (code >> 6)); bytes[size++] = char(0x80 | (code & 63)); }
    else if (code < 65536) {
      bytes[size++] = char(0xe0 | (code >> 12)); bytes[size++] = char(0x80 | ((code >> 6) & 63)); bytes[size++] = char(0x80 | (code & 63));
    } else {
      bytes[size++] = char(0xf0 | (code >> 18)); bytes[size++] = char(0x80 | ((code >> 12) & 63));
      bytes[size++] = char(0x80 | ((code >> 6) & 63)); bytes[size++] = char(0x80 | (code & 63));
    }
    return write(std::string_view(bytes, size));
  }
  constexpr bool getInt(const Value* value, uint32_t& n) {
    return value && value->kind == Kind::Word && integer(value->text, n);
  }
  constexpr bool xrefs(uint32_t position) {
    for (unsigned revision = 0; revision < 16; ++revision) {
    if (position >= io.size(0)) return fail(Error::Invalid);
    Lexer lexer(io, 0, position, io.size(0));
    if (lexer.token().text != "xref") return fail(Error::Unsupported);
    while (io.error == Error::None) {
      const Value first = lexer.token();
      if (first.text == "trailer") break;
      uint32_t start = 0, count = 0;
      if (!getInt(&first, start)) return fail(Error::Invalid);
      const Value length = lexer.token();
      if (!getInt(&length, count) || count > 1000000 || start > UINT32_MAX - count) return fail(Error::Limit);
      // 일반 상호참조 표의 각 항목은 PDF 명세의 고정 20바이트 형식을 따른다.
      while (lexer.peek() == ' ' || lexer.peek() == '\t') lexer.take();
      const int eol = lexer.take();
      if (eol == '\r' && lexer.peek() == '\n') lexer.take();
      else if (eol != '\r' && eol != '\n') return fail(Error::Invalid);
      if (count > (io.size(0) - lexer.position()) / 20) return fail(Error::Invalid);
      if (sections.size() >= 128 || !io.room((sections.size() + 1) * sizeof(Xref) * 2)) return fail(Error::Limit);
      sections.push_back({start, count, lexer.position()});
      lexer.seek(lexer.position() + count * 20);
    }
    Value trailer = lexer.value();
    if (trailer.kind != Kind::Dict) return fail(Error::Invalid);
    const Value* encryption = trailer.get("Encrypt");
    if (encryption && encryption->text != "null") return fail(Error::Encrypted);
    if (trailer.get("XRefStm")) return fail(Error::Unsupported);
    if (!root.id) {
      const Value* r = trailer.get("Root");
      if (r && r->kind == Kind::Ref) root = r->ref;
    }
    uint32_t previous = 0;
    if (trailer.get("Prev")) {
      if (!getInt(trailer.get("Prev"), previous) || previous >= position) return fail(Error::Invalid);
      position = previous;
      continue;
    }
    return io.error == Error::None;
    }
    return fail(Error::Limit);
  }
  constexpr uint32_t offset(Ref ref) {
    for (const auto& section : sections) {
      if (ref.id < section.first || ref.id - section.first >= section.count) continue;
      Lexer lexer(io, 0, section.position + (ref.id - section.first) * 20,
                  section.position + (ref.id - section.first + 1) * 20);
      uint32_t result = 0, generation = 0;
      const Value a = lexer.token(), b = lexer.token(), type = lexer.token();
      if (!getInt(&a, result) || !getInt(&b, generation) || type.text != "n" || generation != ref.generation ||
          result == 0 || result >= io.size(0)) { fail(Error::Invalid); return 0; }
      return result;
    }
    fail(Error::Invalid); return 0;
  }
  constexpr Object object(Ref ref) {
    Object result;
    const uint32_t start = offset(ref);
    if (io.error != Error::None) return result;
    Lexer lexer(io, 0, start, io.size(0));
    uint32_t id = 0, gen = 0;
    const Value a = lexer.token(), b = lexer.token(), marker = lexer.token();
    if (!getInt(&a, id) || !getInt(&b, gen) || id != ref.id || gen != ref.generation || marker.text != "obj") {
      fail(Error::Invalid); return result;
    }
    result.value = lexer.value();
    const Value after = lexer.token();
    if (after.text == "stream" && result.value.kind == Kind::Dict) {
      while (lexer.peek() == ' ' || lexer.peek() == '\t') lexer.take();
      const int eol = lexer.take();
      if (eol == '\r' && lexer.peek() == '\n') lexer.take();
      else if (eol != '\r' && eol != '\n') { fail(Error::Invalid); return {}; }
      result.stream = lexer.position();
    } else if (after.text != "endobj") fail(Error::Invalid);
    return result;
  }
  constexpr size_t copySize(const Value& value) const {
    size_t bytes = value.items.size() * sizeof(Value) + value.text.size() + 32;
    for (const auto& item : value.items) bytes += copySize(item);
    return bytes;
  }
  constexpr Value resolve(const Value& value) {
    if (value.kind == Kind::Ref) return object(value.ref).value;
    // 직접 포함된 리소스 사전은 복사 순간에 원본과 사본의 메모리가 모두 필요하다.
    if (!io.room(copySize(value))) { fail(Error::Memory); return {}; }
    return value;
  }
  constexpr bool stream(const Object& obj, unsigned slot) {
    if (!obj.stream) return fail(Error::Invalid);
    const Value* rawLength = obj.value.get("Length");
    if (!rawLength) return fail(Error::Invalid);
    const Value lengthValue = resolve(*rawLength);
    uint32_t length = 0;
    if (!getInt(&lengthValue, length) || length > io.size(0) - obj.stream) return fail(Error::Invalid);
    const Value* filter = obj.value.get("Filter");
    if (filter && filter->kind == Kind::Array && filter->items.size() == 1) filter = &filter->items[0];
    if (filter && !filter->is("FlateDecode") && !filter->is("Fl")) return fail(Error::Unsupported);
    const Value* params = obj.value.get("DecodeParms");
    if (params && params->text != "null") {
      if (params->kind == Kind::Array && params->items.size() == 1) params = &params->items[0];
      const Value* predictor = params->get("Predictor");
      uint32_t prediction = 1;
      if ((params->kind != Kind::Dict && params->text != "null") ||
          (predictor && (!getInt(predictor, prediction) || prediction != 1))) return fail(Error::Unsupported);
    }
    if (!io.decode(obj.stream, length, filter != nullptr, slot)) return false;
    return io.error == Error::None;
  }
  constexpr bool rawCode(const Value& value, uint64_t& code, size_t maxBytes) {
    code = 0;
    if (value.kind != Kind::String || value.text.empty() || value.text.size() > maxBytes) return fail(Error::Encoding);
    for (unsigned char ch : value.text) code = (code << 8) | ch;
    return true;
  }
  constexpr bool addMap(const Value& first, const Value& lastCode, const Value& target) {
    uint64_t a = 0, b = 0, dst = 0;
    if (!rawCode(first, a, 4) || !rawCode(lastCode, b, 4) || !rawCode(target, dst, 8) ||
        first.text.size() != lastCode.text.size() || a > b || target.text.size() % 2) return fail(Error::Encoding);
    const uint64_t maximum = target.text.size() == 8 ? UINT64_MAX : (uint64_t(1) << (target.text.size() * 8)) - 1;
    if (b - a > maximum - dst) return fail(Error::Encoding);
    if (mappings.size() >= 4096) return fail(Error::Limit);
    if (mappings.size() == mappings.capacity()) {
      const size_t capacity = mappings.size() + 128;
      if (!io.room(capacity * sizeof(Mapping))) return fail(Error::Memory);
      mappings.reserve(capacity);
    }
    mappings.push_back({dst, uint32_t(a), uint32_t(b), uint8_t(first.text.size()), uint8_t(target.text.size())});
    return true;
  }
  constexpr bool cmap(const Object& obj) {
    if (obj.value.get("UseCMap")) return fail(Error::Unsupported);
    if (!stream(obj, 2)) return false;
    Lexer lexer(io, 2, 0, io.size(2));
    uint32_t count = 0;
    while (io.error == Error::None) {
      const Value word = lexer.token();
      if (word.kind == Kind::End) break;
      uint32_t n = 0;
      if (getInt(&word, n)) { count = n; continue; }
      if (word.text == "usecmap") return fail(Error::Unsupported);
      if (word.text != "beginbfchar" && word.text != "beginbfrange" && word.text != "begincodespacerange") continue;
      if (count == 0 || count > 4096) return fail(Error::Limit);
      for (uint32_t i = 0; i < count; ++i) {
        Value first = lexer.token(), second = lexer.token();
        if (word.text == "begincodespacerange") {
          uint64_t a = 0, b = 0;
          if (!rawCode(first, a, 4) || !rawCode(second, b, 4) || a > b || first.text.size() != second.text.size()) return fail(Error::Encoding);
          if (spaces.size() >= 16 || !io.room(512)) return fail(Error::Limit);
          spaces.push_back({uint32_t(a), uint32_t(b), uint8_t(first.text.size())});
        } else if (word.text == "beginbfchar") {
          if (!addMap(first, first, second)) return false;
        } else {
          Value target = lexer.token();
          if (target.text == "[" && target.kind == Kind::Word) {
            uint64_t a = 0, b = 0;
            if (!rawCode(first, a, 4) || !rawCode(second, b, 4) || a > b || b - a >= 4096 || first.text.size() != second.text.size()) return fail(Error::Encoding);
            for (uint64_t code = a; code <= b; ++code) {
              Value source = first;
              for (size_t j = 0; j < source.text.size(); ++j) source.text[source.text.size() - 1 - j] = char(code >> (8 * j));
              if (!addMap(source, source, lexer.token())) return false;
            }
            if (lexer.token().text != "]") return fail(Error::Encoding);
          } else if (!addMap(first, second, target)) return false;
        }
      }
      const auto end = lexer.token().text;
      if (end != (word.text == "beginbfchar" ? "endbfchar" : word.text == "beginbfrange" ? "endbfrange" : "endcodespacerange")) return fail(Error::Encoding);
      count = 0;
    }
    if (mappings.empty() || spaces.empty()) return fail(Error::Encoding);
    std::sort(mappings.begin(), mappings.end(), [](const Mapping& a, const Mapping& b) {
      return a.bytes < b.bytes || (a.bytes == b.bytes && a.first < b.first);
    });
    for (size_t i = 1; i < mappings.size(); ++i) {
      if (mappings[i - 1].bytes == mappings[i].bytes && mappings[i - 1].last >= mappings[i].first) return fail(Error::Encoding);
    }
    return io.error == Error::None;
  }
  constexpr bool font(const Value& resources, std::string_view name) {
    const Value* fontsValue = resources.get("Font");
    if (!fontsValue) return fail(Error::Encoding);
    const Value fonts = resolve(*fontsValue);
    const Value* selected = fonts.get(name);
    if (!selected || selected->kind != Kind::Ref) return fail(Error::Encoding);
    if (selected->ref == fontRef) return true;
    // 문자표와 압축 사전이 동시에 RAM을 차지하지 않도록 이전 문자표부터 반환한다.
    std::vector<Mapping>().swap(mappings);
    spaces.clear();
    const Object objectFont = object(selected->ref);
    const Value* toUnicode = objectFont.value.get("ToUnicode");
    mapped = toUnicode != nullptr;
    winAnsi = false;
    if (toUnicode) {
      if (toUnicode->kind != Kind::Ref || !cmap(object(toUnicode->ref))) return fail(Error::Encoding);
    } else {
      const Value* type = objectFont.value.get("Subtype");
      if (!type || (!type->is("Type1") && !type->is("TrueType"))) return fail(Error::Encoding);
      const Value* encoding = objectFont.value.get("Encoding");
      if (encoding && !encoding->is("WinAnsiEncoding") && !encoding->is("StandardEncoding")) return fail(Error::Encoding);
      winAnsi = encoding && encoding->is("WinAnsiEncoding");
    }
    fontRef = selected->ref;
    return io.error == Error::None;
  }
  constexpr bool text(std::string_view raw) {
    constexpr uint16_t ansi[32] = {0x20ac,0,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,0x2c6,0x2030,0x160,0x2039,0x152,0,0x17d,0,
                                   0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0,0x17e,0x178};
    for (size_t i = 0; i < raw.size();) {
      uint32_t code = static_cast<unsigned char>(raw[i++]);
      if (!mapped) {
        if (!winAnsi && code >= 128) return fail(Error::Encoding);
        if (winAnsi && code >= 128 && code < 160) { code = ansi[code - 128]; if (!code) return fail(Error::Encoding); }
        if (!emitCodepoint(code)) return false;
        continue;
      }
      size_t bytes = 1;
      bool found = false;
      while (bytes <= 4) {
        for (const auto& range : spaces) if (range.bytes == bytes && code >= range.first && code <= range.last) { found = true; break; }
        if (found || bytes == 4 || i == raw.size()) break;
        code = (code << 8) | static_cast<unsigned char>(raw[i++]); ++bytes;
      }
      if (!found) return fail(Error::Encoding);
      size_t low = 0, high = mappings.size();
      while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const Mapping& map = mappings[middle];
        if (map.bytes < bytes || (map.bytes == bytes && map.first <= code)) low = middle + 1;
        else high = middle;
      }
      if (low == 0) return fail(Error::Encoding);
      const Mapping& map = mappings[low - 1];
      if (map.bytes != bytes || code < map.first || code > map.last) return fail(Error::Encoding);
      const uint64_t unicode = map.target + code - map.first;
      for (int shift = (map.targetBytes - 2) * 8; shift >= 0; shift -= 16) {
        uint32_t cp = (unicode >> shift) & 65535;
        if (cp >= 0xd800 && cp <= 0xdbff) {
          if (shift < 16) return fail(Error::Encoding);
          shift -= 16;
          const uint32_t tail = (unicode >> shift) & 65535;
          if (tail < 0xdc00 || tail > 0xdfff) return fail(Error::Encoding);
          cp = 0x10000 + ((cp - 0xd800) << 10) + tail - 0xdc00;
        }
        if (!emitCodepoint(cp)) return false;
      }
    }
    return true;
  }
  struct SavedFont { std::string name; double size; };
  struct TextState {
    bool active = false, hasY = false, image = false;
    double y = 0, size = 12;
    std::string font;
    std::vector<SavedFont> saved;
  };
  constexpr bool content(const Object& obj, const Value& resources, TextState& state) {
    std::vector<Mapping>().swap(mappings);
    spaces.clear();
    fontRef = {};
    if (!stream(obj, 1)) return false;
    Lexer lexer(io, 1, 0, io.size(1));
    std::vector<Value> args;
    while (io.error == Error::None) {
      Value op = lexer.value();
      if (op.kind == Kind::End) break;
      double numeric = 0;
      if (op.kind != Kind::Word || number(op.text, numeric) || op.text == "true" || op.text == "false" || op.text == "null") {
        if (args.size() >= 32 || !io.room(2048)) return fail(Error::Limit);
        args.push_back(std::move(op)); continue;
      }
      auto num = [&](size_t n, double& value) { return n < args.size() && args[n].kind == Kind::Word && number(args[n].text, value); };
      if (op.text == "q") {
        if (state.saved.size() >= 32 || !io.room(2048)) return fail(Error::Limit);
        state.saved.push_back({state.font, state.size});
      } else if (op.text == "Q") {
        if (state.saved.empty()) return fail(Error::Invalid);
        state.font = std::move(state.saved.back().name);
        state.size = state.saved.back().size;
        state.saved.pop_back();
      } else if (op.text == "BT") { state.active = true; state.hasY = false; if (!newline()) return false; }
      else if (op.text == "ET") { state.active = false; if (!newline()) return false; }
      else if (op.text == "BI") return fail(Error::Unsupported);
      else if (op.text == "Do") {
        if (args.size() != 1 || args[0].kind != Kind::Name) return fail(Error::Invalid);
        const Value* objects = resources.get("XObject");
        if (!objects) return fail(Error::Invalid);
        const Value values = resolve(*objects);
        const Value* value = values.get(args[0].text);
        if (!value || value->kind != Kind::Ref) return fail(Error::Invalid);
        const Object xobject = object(value->ref);
        const Value* subtype = xobject.value.get("Subtype");
        if (!subtype || !subtype->is("Image")) return fail(Error::Unsupported);
        state.image = true;
      } else if (op.text == "Tf") {
        if (args.size() != 2 || args[0].kind != Kind::Name || !num(1, state.size)) return fail(Error::Invalid);
        if (args[0].text.size() > 256) return fail(Error::Limit);
        state.font = args[0].text;
      } else if (state.active && (op.text == "Tm" || op.text == "Td" || op.text == "TD")) {
        double y = 0;
        if (!num(op.text == "Tm" ? 5 : 1, y)) return fail(Error::Invalid);
        const double change = op.text == "Tm" ? (state.hasY ? y - state.y : 0) : y;
        const double size = state.size < 0 ? -state.size : state.size;
        if (change > size * 0.25 || change < -size * 0.25) { if (!newline()) return false; }
        else if (state.hasY && !space()) return false;
        state.y = op.text == "Tm" ? y : state.y + y; state.hasY = true;
      } else if (state.active && op.text == "T*") { if (!newline()) return false; }
      else if (state.active && (op.text == "Tj" || op.text == "TJ" || op.text == "'" || op.text == "\"")) {
        if (args.size() != (op.text == "\"" ? 3u : 1u)) return fail(Error::Invalid);
        if (state.font.empty() || !font(resources, state.font) || args.empty()) return fail(Error::Encoding);
        if ((op.text == "'" || op.text == "\"") && !newline()) return false;
        const Value& shown = args.back();
        if (op.text == "TJ") {
          if (shown.kind != Kind::Array) return fail(Error::Invalid);
          for (const Value& part : shown.items) {
            if (part.kind == Kind::String) { if (!text(part.text)) return false; }
            else if (part.kind == Kind::Word && number(part.text, numeric)) { if (numeric < -120 && !space()) return false; }
            else return fail(Error::Invalid);
          }
        } else if (shown.kind != Kind::String || !text(shown.text)) return fail(Error::Invalid);
      }
      args.clear();
    }
    return io.error == Error::None;
  }
  constexpr bool visitPage(const Object& node, const Value& resources) {
    if (++pageCount > 10000) return fail(Error::Limit);
    const Value* contents = node.value.get("Contents");
    TextState state;
    const uint32_t before = characters;
    if (contents && contents->text != "null") {
      if (contents->kind == Kind::Ref) {
        const Object value = object(contents->ref);
        if (value.value.kind == Kind::Array) {
          for (const Value& item : value.value.items)
            if (item.kind != Kind::Ref || !content(object(item.ref), resources, state)) return fail(Error::Invalid);
        } else if (!content(value, resources, state)) return false;
      } else if (contents->kind == Kind::Array) {
        for (const Value& item : contents->items)
          if (item.kind != Kind::Ref || !content(object(item.ref), resources, state)) return fail(Error::Invalid);
      } else return fail(Error::Invalid);
    }
    if (state.image && before == characters) return fail(Error::NoText);
    return newline() && write("\n");
  }
  constexpr bool pageTree(Ref first) {
    // 재귀 호출 대신 힙의 고정 개수 프레임을 사용하여 렌더 작업의 8 KiB 스택을 보호한다.
    struct Frame {
      Ref ref;
      Object node;
      Value ownResources;
      const Value* effective = nullptr;
      size_t child = 0;
      bool entered = false;
    };
    if (!io.room(24 * sizeof(Frame))) return fail(Error::Memory);
    std::vector<Frame> stack;
    stack.reserve(24);
    stack.push_back(Frame{first, {}, {}, nullptr, 0, false});
    Value emptyResources;
    while (!stack.empty() && io.error == Error::None) {
      Frame& frame = stack.back();
      if (!frame.entered) {
        if (++visited > 20000) return fail(Error::Limit);
        for (size_t i = 0; i + 1 < stack.size(); ++i)
          if (stack[i].ref == frame.ref) return fail(Error::Invalid);
        frame.node = object(frame.ref);
        if (io.error != Error::None) return false;
        const Value* resources = frame.node.value.get("Resources");
        if (resources) {
          frame.ownResources = resolve(*resources);
          frame.effective = &frame.ownResources;
        } else frame.effective = stack.size() > 1 ? stack[stack.size() - 2].effective : &emptyResources;
        frame.entered = true;
      }
      const Value* type = frame.node.value.get("Type");
      if (type && type->is("Page")) {
        if (!visitPage(frame.node, *frame.effective)) return false;
        stack.pop_back();
      } else if (type && type->is("Pages")) {
        const Value* kids = frame.node.value.get("Kids");
        if (!kids || kids->kind != Kind::Array) return fail(Error::Invalid);
        if (frame.child == kids->items.size()) { stack.pop_back(); continue; }
        const Value& child = kids->items[frame.child++];
        if (child.kind != Kind::Ref) return fail(Error::Invalid);
        if (stack.size() == 24) return fail(Error::Limit);
        stack.push_back(Frame{child.ref, {}, {}, nullptr, 0, false});
      } else return fail(Error::Invalid);
    }
    return io.error == Error::None;
  }
 public:
  constexpr Parser(Backend& io, Sink& sink) : io(io), sink(sink) {}
  constexpr Error run() {
    if (io.size(0) < 20 || io.size(0) > 512u * 1024 * 1024) { fail(Error::Invalid); return io.error; }
    constexpr std::string_view signature = "%PDF-";
    for (size_t i = 0; i < signature.size(); ++i) if (io.byte(0, i) != signature[i]) { fail(Error::Invalid); return io.error; }
    // 마지막 startxref만 사용하며 이전 개정은 /Prev 연결을 통해 검증한다.
    uint32_t found = 0;
    const uint32_t begin = io.size(0) > 4096 ? io.size(0) - 4096 : 0;
    for (uint32_t i = begin; i + 9 <= io.size(0); ++i) {
      bool match = true;
      for (size_t j = 0; j < 9; ++j) if (io.byte(0, i + j) != std::string_view("startxref")[j]) { match = false; break; }
      if (match) found = i + 9;
    }
    if (!found) { fail(Error::Invalid); return io.error; }
    Lexer lexer(io, 0, found, io.size(0));
    const Value start = lexer.token(); uint32_t position = 0;
    if (!getInt(&start, position) || !xrefs(position) || !root.id) { fail(Error::Invalid); return io.error; }
    const Object catalog = object(root);
    const Value* pages = catalog.value.get("Pages");
    if (!pages || pages->kind != Kind::Ref) { fail(Error::Invalid); return io.error; }
    if (!pageTree(pages->ref)) return io.error;
    // 줄 구분만 출력한 이미지 전용 문서는 성공으로 처리하지 않는다.
    if (!characters) fail(Error::NoText);
    return io.error;
  }
};
}  // namespace PdfText
