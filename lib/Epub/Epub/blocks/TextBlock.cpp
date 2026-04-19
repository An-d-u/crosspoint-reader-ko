#include "TextBlock.h"

#include <algorithm>

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr int kRubyLineExtraPx = 2;
constexpr int kRubyBaseYOffsetPx = 2;
constexpr int kRubyTextLiftPx = 13;
}

bool TextBlock::hasRuby() const {
  for (const auto& rubyText : rubyTexts) {
    if (!rubyText.empty()) {
      return true;
    }
  }
  return false;
}

int TextBlock::getRenderedLineHeight(const GfxRenderer& renderer, const int fontId, const int rubyFontId,
                                     const float lineCompression) const {
  const int baseLineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression);
  if (!hasRuby()) {
    return baseLineHeight;
  }
  return baseLineHeight + kRubyLineExtraPx;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int rubyFontId, const int x,
                       const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() != rubyTexts.size() ||
      words.size() != tokenWidths.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, ruby=%u, widths=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(), (uint32_t)rubyTexts.size(),
            (uint32_t)tokenWidths.size());
    return;
  }

  const int rubyReserve = hasRuby() ? kRubyBaseYOffsetPx : 0;
  const int baseY = y + rubyReserve;

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const int tokenWidth = tokenWidths[i];
    const int baseWidth = renderer.getTextAdvanceX(fontId, words[i].c_str(), currentStyle);
    const int centeredBaseX = wordX + std::max(0, (tokenWidth - baseWidth) / 2);
    renderer.drawText(fontId, centeredBaseX, baseY, words[i].c_str(), true, currentStyle);

    if (!rubyTexts[i].empty()) {
      const int rubyWidth = renderer.getTextAdvanceX(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR);
      const int baseCenterX = centeredBaseX + (baseWidth / 2);
      const int centeredRubyX = baseCenterX - (rubyWidth / 2);
      const int rubyY = y - kRubyTextLiftPx;
      renderer.drawText(rubyFontId, centeredRubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
      // baseY is the top of the base text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = baseY + renderer.getFontAscenderSize(fontId) + 2;

      int startX = centeredBaseX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() != rubyTexts.size() ||
      words.size() != tokenWidths.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, ruby=%u, widths=%u)\n",
            words.size(), wordXpos.size(), wordStyles.size(), rubyTexts.size(), tokenWidths.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (const auto& rubyText : rubyTexts) serialization::writeString(file, rubyText);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto width : tokenWidths) serialization::writePod(file, width);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<std::string> rubyTexts;
  std::vector<int16_t> wordXpos;
  std::vector<uint16_t> tokenWidths;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  rubyTexts.resize(wc);
  wordXpos.resize(wc);
  tokenWidths.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& rubyText : rubyTexts) serialization::readString(file, rubyText);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& width : tokenWidths) serialization::readPod(file, width);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);

  return std::unique_ptr<TextBlock>(
      new TextBlock(std::move(words), std::move(rubyTexts), std::move(wordXpos), std::move(tokenWidths),
                    std::move(wordStyles), blockStyle));
}
