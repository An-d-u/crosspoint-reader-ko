#include "TextBlock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr int kRubyLineExtraPx = 2;
constexpr int kRubyBaseYOffsetPx = 2;
constexpr int kRubyTextLiftPx = 19;

struct RubyOverlayRun {
  const char* text;
  int preferredX;
  int x;
  int y;
  int width;
};

void drawRubyText(const GfxRenderer& renderer, const int rubyFontId, const RubyOverlayRun& rubyRun) {
  renderer.drawText(rubyFontId, rubyRun.x, rubyRun.y, rubyRun.text, true, EpdFontFamily::REGULAR);
}

void placeRubyCluster(std::vector<RubyOverlayRun>& rubyRuns, const size_t start, const size_t endExclusive) {
  if (start >= endExclusive) {
    return;
  }

  if (endExclusive - start == 1) {
    rubyRuns[start].x = rubyRuns[start].preferredX;
    return;
  }

  double weightedCenterSum = 0.0;
  int totalWeight = 0;
  int totalWidth = 0;
  for (size_t i = start; i < endExclusive; ++i) {
    const int weight = std::max(1, rubyRuns[i].width);
    weightedCenterSum += (rubyRuns[i].preferredX + rubyRuns[i].width / 2.0) * weight;
    totalWeight += weight;
    totalWidth += rubyRuns[i].width;
  }

  const double clusterCenter = weightedCenterSum / std::max(1, totalWeight);
  int currentX = static_cast<int>(std::lround(clusterCenter - totalWidth / 2.0));
  for (size_t i = start; i < endExclusive; ++i) {
    rubyRuns[i].x = currentX;
    currentX += rubyRuns[i].width;
  }
}

std::vector<RubyOverlayRun> buildRubyOverlayRuns(const GfxRenderer& renderer, const int rubyFontId,
                                                 const std::vector<RubyAnnotation>& rubyAnnotations,
                                                 const std::vector<int16_t>& wordXpos,
                                                 const std::vector<uint16_t>& tokenWidths, const int x,
                                                 const int y) {
  std::vector<RubyOverlayRun> rubyRuns;
  rubyRuns.reserve(rubyAnnotations.size());

  for (const auto& ruby : rubyAnnotations) {
    if (ruby.text.empty() || ruby.wordCount == 0 || ruby.startWordIndex >= wordXpos.size()) {
      continue;
    }

    const size_t firstIndex = ruby.startWordIndex;
    const size_t lastIndex = std::min(wordXpos.size() - 1, firstIndex + ruby.wordCount - 1);
    const int baseX = wordXpos[firstIndex] + x;
    const int baseRight = wordXpos[lastIndex] + x + tokenWidths[lastIndex];
    const int baseWidth = baseRight - baseX;
    const int rubyWidth = renderer.getTextAdvanceX(rubyFontId, ruby.text.c_str(), EpdFontFamily::REGULAR);
    const int preferredX = baseX + (baseWidth - rubyWidth) / 2;
    rubyRuns.push_back({ruby.text.c_str(), preferredX, preferredX, y - kRubyTextLiftPx, rubyWidth});
  }

  return rubyRuns;
}

void resolveRubyRunOverlaps(std::vector<RubyOverlayRun>& rubyRuns) {
  if (rubyRuns.size() < 2) {
    return;
  }

  size_t clusterStart = 0;
  while (clusterStart < rubyRuns.size()) {
    size_t clusterEnd = clusterStart + 1;
    int preferredRight = rubyRuns[clusterStart].preferredX + rubyRuns[clusterStart].width;
    while (clusterEnd < rubyRuns.size() && rubyRuns[clusterEnd].preferredX < preferredRight) {
      preferredRight = std::max(preferredRight, rubyRuns[clusterEnd].preferredX + rubyRuns[clusterEnd].width);
      ++clusterEnd;
    }
    placeRubyCluster(rubyRuns, clusterStart, clusterEnd);
    clusterStart = clusterEnd;
  }

  bool merged = true;
  while (merged) {
    merged = false;
    for (size_t i = 1; i < rubyRuns.size(); ++i) {
      if (rubyRuns[i - 1].x + rubyRuns[i - 1].width <= rubyRuns[i].x) {
        continue;
      }

      size_t overlapStart = i - 1;
      while (overlapStart > 0 && rubyRuns[overlapStart - 1].x + rubyRuns[overlapStart - 1].width > rubyRuns[overlapStart].x) {
        --overlapStart;
      }

      size_t overlapEnd = i + 1;
      while (overlapEnd < rubyRuns.size() && rubyRuns[overlapEnd - 1].x + rubyRuns[overlapEnd - 1].width > rubyRuns[overlapEnd].x) {
        ++overlapEnd;
      }

      placeRubyCluster(rubyRuns, overlapStart, overlapEnd);
      merged = true;
      break;
    }
  }
}

void clampRubyRunsToScreen(std::vector<RubyOverlayRun>& rubyRuns, const int screenWidth) {
  if (screenWidth <= 0) {
    return;
  }

  for (auto& rubyRun : rubyRuns) {
    if (rubyRun.width >= screenWidth) {
      rubyRun.x = 0;
      continue;
    }
    rubyRun.x = std::max(0, std::min(rubyRun.x, screenWidth - rubyRun.width));
  }
}
}

bool TextBlock::hasRuby() const {
  for (const auto& ruby : rubyAnnotations) {
    if (!ruby.text.empty()) {
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
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != tokenWidths.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, widths=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)tokenWidths.size());
    return;
  }

  const bool blockHasRuby = hasRuby();
  if (!blockHasRuby) {
    for (size_t i = 0; i < words.size(); i++) {
      const int wordX = wordXpos[i] + x;
      const EpdFontFamily::Style currentStyle = wordStyles[i];
      renderer.drawText(fontId, wordX, y, words[i].c_str(), true, currentStyle);

      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const std::string& w = words[i];
        const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
        // y is the top of the text line; add ascender to reach baseline, then offset 2px below
        const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;

        int startX = wordX;
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
    return;
  }

  const int rubyReserve = kRubyBaseYOffsetPx;
  const int baseY = y + rubyReserve;
  auto rubyRuns = buildRubyOverlayRuns(renderer, rubyFontId, rubyAnnotations, wordXpos, tokenWidths, x, y);

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    renderer.drawText(fontId, wordX, baseY, words[i].c_str(), true, currentStyle);

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
      // baseY is the top of the base text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = baseY + renderer.getFontAscenderSize(fontId) + 2;

      int startX = wordX;
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

  resolveRubyRunOverlaps(rubyRuns);
  clampRubyRunsToScreen(rubyRuns, renderer.getScreenWidth());
  for (const auto& rubyRun : rubyRuns) {
    drawRubyText(renderer, rubyFontId, rubyRun);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != tokenWidths.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, widths=%u)\n",
            words.size(), wordXpos.size(), wordStyles.size(), tokenWidths.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto width : tokenWidths) serialization::writePod(file, width);
  for (auto s : wordStyles) serialization::writePod(file, s);

  serialization::writePod(file, static_cast<uint16_t>(rubyAnnotations.size()));
  for (const auto& ruby : rubyAnnotations) {
    serialization::writePod(file, ruby.startWordIndex);
    serialization::writePod(file, ruby.wordCount);
    serialization::writeString(file, ruby.text);
  }

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
  std::vector<RubyAnnotation> rubyAnnotations;
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
  wordXpos.resize(wc);
  tokenWidths.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& width : tokenWidths) serialization::readPod(file, width);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  uint16_t rubyCount = 0;
  serialization::readPod(file, rubyCount);
  rubyAnnotations.resize(rubyCount);
  for (auto& ruby : rubyAnnotations) {
    serialization::readPod(file, ruby.startWordIndex);
    serialization::readPod(file, ruby.wordCount);
    serialization::readString(file, ruby.text);
  }

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
      new TextBlock(std::move(words), std::move(rubyAnnotations), std::move(wordXpos), std::move(tokenWidths),
                    std::move(wordStyles), blockStyle));
}
