#include "PdfTextParser.h"

#include <array>

namespace {
constexpr std::string decimal(uint32_t value, unsigned width = 0) {
  std::string result;
  do { result.insert(result.begin(), char('0' + value % 10)); value /= 10; } while (value);
  while (result.size() < width) result.insert(result.begin(), '0');
  return result;
}
constexpr std::string stream(std::string_view contents) {
  return "<< /Length " + decimal(contents.size()) + " >>\nstream\n" + std::string(contents) + "\nendstream";
}
constexpr std::string document(const std::vector<std::string>& objects, std::string_view trailer = "") {
  std::string data = "%PDF-1.4\n";
  std::vector<uint32_t> offsets{0};
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(data.size());
    data += decimal(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const auto xref = data.size();
  data += "xref\n0 " + decimal(offsets.size()) + "\n0000000000 65535 f \n";
  for (size_t i = 1; i < offsets.size(); ++i) data += decimal(offsets[i], 10) + " 00000 n \n";
  data += "trailer\n<< /Size " + decimal(offsets.size()) + " /Root 1 0 R " + std::string(trailer) +
          " >>\nstartxref\n" + decimal(xref) + "\n%%EOF\n";
  return data;
}
constexpr std::vector<std::string> simple(std::string_view text) {
  return {"<< /Type /Catalog /Pages 2 0 R >>",
          "<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /Font << /F1 4 0 R >> >> >>",
          "<< /Type /Page /Parent 2 0 R /Contents 5 0 R >>",
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
          stream(text)};
}
struct Backend {
  PdfText::Error error = PdfText::Error::None;
  std::array<std::string, 3> sources;
  size_t budget = 1000000;
  int failSource = -1;
  constexpr explicit Backend(std::string data) { sources[0] = std::move(data); }
  constexpr uint32_t size(unsigned slot) const { return sources[slot].size(); }
  constexpr int byte(unsigned slot, uint32_t offset) {
    if (static_cast<int>(slot) == failSource) { error = PdfText::Error::Io; return -1; }
    return offset < sources[slot].size() ? static_cast<unsigned char>(sources[slot][offset]) : -1;
  }
  constexpr bool room(size_t bytes) const { return bytes < budget; }
  constexpr bool decode(uint32_t offset, uint32_t length, bool compressed, unsigned slot) {
    if (compressed) { error = PdfText::Error::Unsupported; return false; }
    sources[slot] = sources[0].substr(offset, length);
    return true;
  }
};
struct Result { PdfText::Error error; std::string text; };
constexpr Result extract(std::string data, size_t budget = 1000000, bool writeError = false, int failSource = -1) {
  Backend io(std::move(data));
  io.budget = budget;
  io.failSource = failSource;
  std::string output;
  auto sink = [&](std::string_view text) { output += text; return !writeError; };
  PdfText::Parser parser(io, sink);
  return {parser.run(), output};
}

constexpr bool textOperators() {
  const auto result = extract(document(simple("q BT /F1 12 Tf 3 Tr 1 0 0 1 20 700 Tm (First\\040line) Tj "
      "0 -20 Td [(Second) -250 (part)] TJ T* (third \\(line\\)) Tj (fourth) ' 0 0 (fifth) \" ET Q")));
  return result.error == PdfText::Error::None && result.text == "First line\nSecond part\nthird (line)\nfourth\nfifth\n\n";
}
static_assert(textOperators());

constexpr bool unicode() {
  auto objects = simple("BT /F1 12 Tf <00010002000300040005> Tj ET");
  objects[3] = "<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /ToUnicode 6 0 R >>";
  objects.push_back(stream("1 begincodespacerange <0000> <ffff> endcodespacerange "
      "2 beginbfchar <0001> <AC00> <0002> <65E5> endbfchar "
      "1 beginbfrange <0003> <0005> [<00660069> <D840DC00> <0065>] endbfrange"));
  const auto result = extract(document(objects));
  return result.error == PdfText::Error::None && result.text == "가日fi𠀀e\n\n";
}
static_assert(unicode());

constexpr bool sequenceRange() {
  auto objects = simple("BT /F1 12 Tf <010203> Tj ET");
  objects[3] = "<< /Subtype /TrueType /ToUnicode 6 0 R >>";
  objects.push_back(stream("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfrange <01> <03> <0041> endbfrange"));
  const auto result = extract(document(objects));
  return result.error == PdfText::Error::None && result.text == "ABC\n\n";
}
static_assert(sequenceRange());

constexpr bool pageOrder() {
  auto objects = simple("BT /F1 12 Tf (later) Tj ET");
  objects[1] = "<< /Type /Pages /Kids [6 0 R 3 0 R] /Count 2 /Resources << /Font << /F1 4 0 R >> >> >>";
  objects.push_back("<< /Type /Page /Parent 2 0 R /Contents [7 0 R 8 0 R] >>");
  objects.push_back(stream("BT /F1 12 Tf (early) Tj"));
  objects.push_back(stream("( part) Tj ET"));
  const auto result = extract(document(objects));
  return result.error == PdfText::Error::None && result.text == "early part\n\nlater\n\n";
}
static_assert(pageOrder());

constexpr bool incrementalUpdate() {
  std::string data = document(simple("BT /F1 12 Tf (old) Tj ET"));
  const auto previous = data.find("xref\n");
  const auto updated = data.size();
  data += "5 0 obj\n" + stream("BT /F1 12 Tf (updated) Tj ET") + "\nendobj\n";
  const auto latest = data.size();
  data += "xref\n5 1\n" + decimal(updated, 10) + " 00000 n \ntrailer\n<< /Size 6 /Prev " +
          decimal(previous) + " >>\nstartxref\n" + decimal(latest) + "\n%%EOF\n";
  const auto result = extract(data);
  if (result.error != PdfText::Error::None || result.text != "updated\n\n") return false;
  // 새 개정에서 삭제한 객체를 과거 개정에서 되살려 읽으면 안 된다.
  data[latest + std::string_view("xref\n5 1\n").size() + 17] = 'f';
  return extract(data).error == PdfText::Error::Invalid;
}
static_assert(incrementalUpdate());

constexpr bool restoredFont() {
  auto objects = simple("BT /F1 12 Tf (A) Tj q /F2 12 Tf <01> Tj Q (B) Tj ET");
  objects[1] = "<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> >>";
  objects.push_back("<< /Subtype /TrueType /ToUnicode 7 0 R >>");
  objects.push_back(stream("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfchar <01> <65E5> endbfchar"));
  const auto result = extract(document(objects));
  return result.error == PdfText::Error::None && result.text == "A日B\n\n";
}
static_assert(restoredFont());

constexpr bool imageOcr() {
  auto objects = simple("q /Im1 Do Q BT /F1 12 Tf 3 Tr (hidden OCR) Tj ET");
  objects[2] = "<< /Type /Page /Parent 2 0 R /Contents 5 0 R /Resources << /Font << /F1 4 0 R >> /XObject << /Im1 6 0 R >> >> >>";
  objects.push_back("<< /Type /XObject /Subtype /Image /Filter /DCTDecode /Length 7 >>\nstream\nignored\nendstream");
  auto result = extract(document(objects));
  if (result.error != PdfText::Error::None || result.text != "hidden OCR\n\n") return false;
  objects[4] = stream("q /Im1 Do Q");
  return extract(document(objects)).error == PdfText::Error::NoText;
}
static_assert(imageOcr());

constexpr bool safeErrors() {
  if (extract("not a PDF").error != PdfText::Error::Invalid) return false;
  auto objects = simple("BT /F1 12 Tf (text) Tj ET");
  if (extract(document(objects, "/Encrypt 99 0 R")).error != PdfText::Error::Encrypted) return false;
  if (extract(document(objects, "/XRefStm 20")).error != PdfText::Error::Unsupported) return false;
  if (extract(document(objects), 1000000, true).error != PdfText::Error::Io) return false;
  if (extract(document(objects), 1000000, false, 1).error != PdfText::Error::Io) return false;
  if (extract(document(objects), 64).error == PdfText::Error::None) return false;
  objects[1] = "<< /Type /Pages /Kids [2 0 R] /Count 1 >>";
  if (extract(document(objects)).error != PdfText::Error::Invalid) return false;
  objects = simple("BT /F1 12 Tf <0001> Tj ET");
  objects[3] = "<< /Subtype /Type0 /Encoding /Identity-H >>";
  if (extract(document(objects)).error != PdfText::Error::Encoding) return false;
  objects = simple("q Q");
  return extract(document(objects)).error == PdfText::Error::NoText;
}
static_assert(safeErrors());

constexpr bool malformedMap() {
  auto objects = simple("BT /F1 12 Tf <01> Tj ET");
  objects[3] = "<< /Subtype /TrueType /ToUnicode 6 0 R >>";
  objects.push_back(stream("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfchar <01> <D800> endbfchar"));
  if (extract(document(objects)).error != PdfText::Error::Encoding) return false;
  objects[5] = stream("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfchar <01> <0000> endbfchar");
  if (extract(document(objects)).error != PdfText::Error::Encoding) return false;
  objects[5] = stream("1 begincodespacerange <00> <ff> endcodespacerange 1 beginbfchar <02> <0041> endbfchar");
  return extract(document(objects)).error == PdfText::Error::Encoding;
}
static_assert(malformedMap());

constexpr bool tokenBounds() {
  Backend io("(a\\\r\nb\\101(c)) <4142f> /F#31");
  PdfText::Lexer lexer(io, 0, 0, io.size(0));
  if (lexer.token().text != "abA(c)" || lexer.token().text != std::string("AB\xf0", 3) || lexer.token().text != "F1") return false;
  Backend bad("(unterminated");
  PdfText::Lexer invalid(bad, 0, 0, bad.size(0));
  invalid.token();
  if (bad.error != PdfText::Error::Invalid) return false;
  Backend large("(" + std::string(4097, 'a') + ")");
  PdfText::Lexer limited(large, 0, 0, large.size(0));
  limited.token();
  if (large.error != PdfText::Error::Limit) return false;
  Backend nested("[[[[[[[[[[0]]]]]]]]]]");
  PdfText::Lexer depth(nested, 0, 0, nested.size(0));
  depth.value();
  return nested.error == PdfText::Error::Limit;
}
static_assert(tokenBounds());
}  // namespace
