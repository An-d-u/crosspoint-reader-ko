#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {
constexpr int kRubyContinuationTightenPx = 10;

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

uint16_t measureTokenWidth(const GfxRenderer& renderer, const int fontId, const int rubyFontId, const std::string& word,
                          const std::string& rubyText, const EpdFontFamily::Style style,
                          const bool appendHyphen = false) {
  const uint16_t baseWidth = measureWordWidth(renderer, fontId, word, style, appendHyphen);
  (void)rubyFontId;
  (void)rubyText;
  return baseWidth;
}

uint16_t measureRubyWidth(const GfxRenderer& renderer, const int rubyFontId, const std::string& rubyText) {
  if (rubyText.empty()) {
    return 0;
  }
  return renderer.getTextAdvanceX(rubyFontId, rubyText.c_str(), EpdFontFamily::REGULAR);
}

int getRubyPairExtraGap(const uint16_t leftBaseWidth, const uint16_t leftRubyWidth, const uint16_t rightBaseWidth,
                        const uint16_t rightRubyWidth, const int currentGap) {
  if (leftRubyWidth == 0 || rightRubyWidth == 0) {
    return 0;
  }

  const int rubyOverhangDelta =
      static_cast<int>(leftRubyWidth) + static_cast<int>(rightRubyWidth) - static_cast<int>(leftBaseWidth) -
      static_cast<int>(rightBaseWidth);
  if (rubyOverhangDelta <= 0) {
    return 0;
  }

  const int requiredGap = (rubyOverhangDelta + 1) / 2;
  return std::max(0, requiredGap - currentGap);
}

int getRubyContinuationTighten(const std::vector<std::string>& rubyTexts, const size_t nextIndex) {
  if (nextIndex == 0 || nextIndex >= rubyTexts.size()) {
    return 0;
  }
  if (rubyTexts[nextIndex - 1].empty() && rubyTexts[nextIndex].empty()) {
    return 0;
  }
  return kRubyContinuationTightenPx;
}

}  // namespace

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, std::string rubyText) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  rubyTexts.push_back(std::move(rubyText));
  EpdFontFamily::Style combinedStyle = fontStyle;
  if (underline) {
    combinedStyle = static_cast<EpdFontFamily::Style>(combinedStyle | EpdFontFamily::UNDERLINE);
  }
  wordStyles.push_back(combinedStyle);
  wordContinues.push_back(attachToPrevious);
}

// Helper function to split a UTF-8 string into individual characters
static std::vector<std::string> splitUtf8Chars(const std::string& str) {
  std::vector<std::string> chars;
  const char* p = str.c_str();
  while (*p) {
    int charLen = 1;
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c & 0xF8) == 0xF0) {
      charLen = 4;
    } else if ((c & 0xF0) == 0xE0) {
      charLen = 3;
    } else if ((c & 0xE0) == 0xC0) {
      charLen = 2;
    }
    chars.push_back(std::string(p, charLen));
    p += charLen;
  }
  return chars;
}

// Character-wrap mode: greedy line filling with justified alignment (1.0x-1.5x spacing)
// If spacing would exceed 1.5x, split words at character boundaries to fill the line
void ParsedText::layoutCharacterWrap(const GfxRenderer& renderer, const int fontId, const int rubyFontId,
                                     const uint16_t viewportWidth, const int spaceWidth,
                                     const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                     const bool includeLastLine) {
  const int pageWidth = viewportWidth;
  // Spacing range: 1.0x to 1.5x of normal space width
  const int minSpacing = spaceWidth;
  const int maxSpacing = spaceWidth + (spaceWidth / 2);  // 1.5x

  // Add paragraph indent to first word
  if ((blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) &&
      paragraphIndent && !words.empty()) {
    words.front().insert(0, "\xe3\x80\x80");  // U+3000 ideographic space
  }

  while (!words.empty()) {
    std::vector<std::string> lineWordsVec;
    std::vector<std::string> lineRubyTextsVec;
    std::vector<bool> lineWordContinuesVec;
    std::vector<int> lineWordWidths;
    std::vector<int> lineRubyWidths;
    std::vector<EpdFontFamily::Style> lineWordStylesVec;

    // Phase 1: Greedily collect words/characters to fill the line
    // Target: spacing should be between minSpacing and maxSpacing
    int totalWordWidth = 0;

    while (!words.empty()) {
      const std::string& word = words.front();
      const std::string& rubyText = rubyTexts.front();
      const EpdFontFamily::Style wordStyle = wordStyles.front();
      const int wordWidth = measureTokenWidth(renderer, fontId, rubyFontId, word, rubyText, wordStyle);
      const int rubyWidth = measureRubyWidth(renderer, rubyFontId, rubyText);
      const int rubyPairExtra =
          lineWordsVec.empty() ? 0
                               : getRubyPairExtraGap(static_cast<uint16_t>(lineWordWidths.back()),
                                                     static_cast<uint16_t>(lineRubyWidths.back()),
                                                     static_cast<uint16_t>(wordWidth), static_cast<uint16_t>(rubyWidth), 0);

      // Calculate what spacing would be if we add this word
      int newTotalWidth = totalWordWidth + wordWidth + rubyPairExtra;
      int newGapCount = lineWordsVec.size();  // gaps = word count (before adding new word)
      int newSpareSpace = pageWidth - newTotalWidth;
      int newSpacing = (newGapCount > 0) ? (newSpareSpace / newGapCount) : maxSpacing + 1;

      if (lineWordsVec.empty()) {
        // First word - must add something
        if (wordWidth <= pageWidth) {
          // Whole word fits
          lineWordsVec.push_back(word);
          lineRubyTextsVec.push_back(rubyText);
          lineWordContinuesVec.push_back(false);
          lineWordWidths.push_back(wordWidth);
          lineRubyWidths.push_back(rubyWidth);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = wordWidth;
          words.erase(words.begin());
          rubyTexts.erase(rubyTexts.begin());
          wordContinues.erase(wordContinues.begin());
          wordStyles.erase(wordStyles.begin());
        } else {
          if (!rubyText.empty()) {
            lineWordsVec.push_back(word);
            lineRubyTextsVec.push_back(rubyText);
            lineWordContinuesVec.push_back(false);
            lineWordWidths.push_back(wordWidth);
            lineRubyWidths.push_back(rubyWidth);
            lineWordStylesVec.push_back(wordStyle);
            totalWordWidth = wordWidth;
            words.erase(words.begin());
            rubyTexts.erase(rubyTexts.begin());
            wordContinues.erase(wordContinues.begin());
            wordStyles.erase(wordStyles.begin());
            break;
          }
          // Word too long - split it
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;
          for (size_t i = 0; i < chars.size(); i++) {
            std::string test = partial + chars[i];
            int testWidth = renderer.getTextWidth(fontId, test.c_str(), wordStyle);
            if (testWidth > pageWidth) break;
            partial = test;
            charsFit = i + 1;
          }
          if (charsFit == 0) {
            charsFit = 1;
            partial = chars[0];
          }
          int partialWidth = renderer.getTextWidth(fontId, partial.c_str(), wordStyle);
          const bool attachToPrevious = wordContinues.front();
          lineWordsVec.push_back(partial);
          lineRubyTextsVec.emplace_back();
          lineWordContinuesVec.push_back(attachToPrevious);
          lineWordWidths.push_back(partialWidth);
          lineRubyWidths.push_back(0);
          lineWordStylesVec.push_back(wordStyle);
          totalWordWidth = partialWidth;

          if (charsFit < chars.size()) {
            std::string remainder;
            for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
            words.front() = remainder;
          } else {
            words.erase(words.begin());
            rubyTexts.erase(rubyTexts.begin());
            wordContinues.erase(wordContinues.begin());
            wordStyles.erase(wordStyles.begin());
          }
        }
      } else if (newSpacing >= minSpacing) {
        // Adding this word keeps spacing >= minSpacing - add it
        const bool attachToPrevious = wordContinues.front();
        lineWordsVec.push_back(word);
        lineRubyTextsVec.push_back(rubyText);
        lineWordContinuesVec.push_back(attachToPrevious);
        lineWordWidths.push_back(wordWidth);
        lineRubyWidths.push_back(rubyWidth);
        lineWordStylesVec.push_back(wordStyle);
        totalWordWidth = newTotalWidth;
        words.erase(words.begin());
        rubyTexts.erase(rubyTexts.begin());
        wordContinues.erase(wordContinues.begin());
        wordStyles.erase(wordStyles.begin());

        // If spacing is now within range, we might be done with this line
        if (newSpacing <= maxSpacing) {
          // Perfect! But check if we can fit more
          continue;
        }
      } else {
        // Adding whole word would make spacing < minSpacing
        // Try to add partial characters from this word
        int currentGapCount = lineWordsVec.size();
        // We want: (pageWidth - totalWordWidth - partialWidth) / currentGapCount >= minSpacing
        // So: partialWidth <= pageWidth - totalWordWidth - currentGapCount * minSpacing
        int maxPartialWidth = pageWidth - totalWordWidth - currentGapCount * minSpacing;

        if (maxPartialWidth > 0) {
          if (!rubyText.empty()) {
            break;
          }
          auto chars = splitUtf8Chars(word);
          std::string partial;
          size_t charsFit = 0;
          for (size_t i = 0; i < chars.size(); i++) {
            std::string test = partial + chars[i];
            int testWidth = renderer.getTextWidth(fontId, test.c_str(), wordStyle);
            if (testWidth > maxPartialWidth) break;
            partial = test;
            charsFit = i + 1;
          }

          if (charsFit > 0) {
            int partialWidth = renderer.getTextWidth(fontId, partial.c_str(), wordStyle);
            const bool attachToPrevious = wordContinues.front();
            lineWordsVec.push_back(partial);
            lineRubyTextsVec.emplace_back();
            lineWordContinuesVec.push_back(attachToPrevious);
            lineWordWidths.push_back(partialWidth);
            lineRubyWidths.push_back(0);
            lineWordStylesVec.push_back(wordStyle);
            totalWordWidth += partialWidth;

            if (charsFit < chars.size()) {
              std::string remainder;
              for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
              words.front() = remainder;
            } else {
              words.erase(words.begin());
              rubyTexts.erase(rubyTexts.begin());
              wordContinues.erase(wordContinues.begin());
              wordStyles.erase(wordStyles.begin());
            }
          }
        }
        // Line is full
        break;
      }
    }

    // Phase 2: Check if spacing is too large, fill with more characters
    while (!words.empty() && lineWordsVec.size() >= 1) {
      int gapCount = lineWordsVec.size();
      int spareSpace = pageWidth - totalWordWidth;
      int spacing = (gapCount > 0) ? (spareSpace / gapCount) : 0;

      if (spacing <= maxSpacing) break;  // Spacing is acceptable

      // Spacing too large - try to add characters from next word
      const std::string& nextWord = words.front();
      const std::string& nextRubyText = rubyTexts.front();
      const EpdFontFamily::Style nextStyle = wordStyles.front();
      if (!nextRubyText.empty()) break;
      auto chars = splitUtf8Chars(nextWord);

      // Calculate max width for partial word to keep spacing <= maxSpacing
      // (pageWidth - totalWordWidth - partialWidth) / gapCount <= maxSpacing
      // partialWidth >= pageWidth - totalWordWidth - gapCount * maxSpacing
      int minPartialWidth = pageWidth - totalWordWidth - gapCount * maxSpacing;
      // Also ensure spacing >= minSpacing after adding
      // (pageWidth - totalWordWidth - partialWidth) / gapCount >= minSpacing
      // partialWidth <= pageWidth - totalWordWidth - gapCount * minSpacing
      int maxPartialWidth = pageWidth - totalWordWidth - gapCount * minSpacing;

      if (maxPartialWidth <= 0) break;  // Can't fit anything

      std::string partial;
      int partialWidth = 0;
      size_t charsFit = 0;
      for (size_t i = 0; i < chars.size(); i++) {
        std::string test = partial + chars[i];
        int testWidth = renderer.getTextWidth(fontId, test.c_str(), nextStyle);
        if (testWidth > maxPartialWidth) break;
        partial = test;
        partialWidth = testWidth;
        charsFit = i + 1;
      }

      if (charsFit == 0) break;  // Can't fit any character

      // Add partial
      const bool attachToPrevious = wordContinues.front();
      lineWordsVec.push_back(partial);
      lineRubyTextsVec.emplace_back();
      lineWordContinuesVec.push_back(attachToPrevious);
      lineWordWidths.push_back(partialWidth);
      lineRubyWidths.push_back(0);
      lineWordStylesVec.push_back(nextStyle);
      totalWordWidth += partialWidth;

      if (charsFit < chars.size()) {
        std::string remainder;
        for (size_t i = charsFit; i < chars.size(); i++) remainder += chars[i];
        words.front() = remainder;
      } else {
        words.erase(words.begin());
        rubyTexts.erase(rubyTexts.begin());
        wordContinues.erase(wordContinues.begin());
        wordStyles.erase(wordStyles.begin());
      }
    }

    // Phase 3: Calculate final positions for justified alignment
    bool isLastLine = words.empty();
    int gapCount = lineWordsVec.size() - 1;
    int spareSpace = pageWidth - totalWordWidth;

    std::vector<std::string> lineWords;
    std::vector<std::string> lineRubyTexts;
    std::vector<int16_t> lineXPos;
    std::vector<EpdFontFamily::Style> lineWordStyles;

    if (isLastLine || gapCount <= 0) {
      // Last line or single word: left align with normal spacing
      int xpos = 0;
      for (size_t i = 0; i < lineWordsVec.size(); i++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWords.push_back(lineWordsVec[i]);
        lineRubyTexts.push_back(lineRubyTextsVec[i]);
        lineWordStyles.push_back(lineWordStylesVec[i]);
        if (i < lineWordsVec.size() - 1) {
          int gap = minSpacing;
          if (lineWordContinuesVec[i + 1]) {
            gap = std::max(0, gap - getRubyContinuationTighten(lineRubyTextsVec, i + 1));
          }
          const int rubyPairExtra =
              getRubyPairExtraGap(static_cast<uint16_t>(lineWordWidths[i]), static_cast<uint16_t>(lineRubyWidths[i]),
                                  static_cast<uint16_t>(lineWordWidths[i + 1]),
                                  static_cast<uint16_t>(lineRubyWidths[i + 1]), gap);
          xpos += lineWordWidths[i] + gap + rubyPairExtra;
        }
      }
    } else {
      // Justified: distribute spare space evenly across gaps
      // Use fixed-point arithmetic for even distribution
      int baseSpacing = spareSpace / gapCount;
      int extraPixels = spareSpace % gapCount;  // Distribute these across first N gaps

      int xpos = 0;
      for (size_t i = 0; i < lineWordsVec.size(); i++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));
        lineWords.push_back(lineWordsVec[i]);
        lineRubyTexts.push_back(lineRubyTextsVec[i]);
        lineWordStyles.push_back(lineWordStylesVec[i]);

        if (i < lineWordsVec.size() - 1) {
          int gap = baseSpacing + (static_cast<int>(i) < extraPixels ? 1 : 0);
          if (lineWordContinuesVec[i + 1]) {
            gap = std::max(0, gap - getRubyContinuationTighten(lineRubyTextsVec, i + 1));
          }
          const int rubyPairExtra =
              getRubyPairExtraGap(static_cast<uint16_t>(lineWordWidths[i]), static_cast<uint16_t>(lineRubyWidths[i]),
                                  static_cast<uint16_t>(lineWordWidths[i + 1]),
                                  static_cast<uint16_t>(lineRubyWidths[i + 1]), gap);
          xpos += lineWordWidths[i] + gap + rubyPairExtra;
        }
      }
    }

    // Process the line
    if (!lineWords.empty() && (!isLastLine || includeLastLine)) {
      BlockStyle lineBlockStyle;
      lineBlockStyle.alignment = isLastLine ? CssTextAlign::Left : CssTextAlign::Justify;
      processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineRubyTexts), std::move(lineXPos),
                                              std::vector<uint16_t>(lineWordWidths.begin(), lineWordWidths.end()),
                                              std::move(lineWordStyles), lineBlockStyle));
    }
  }
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const int rubyFontId,
                                       const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Apply fixed transforms before any per-line layout work.
  applyParagraphIndent();

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(fontId);

  // Korean: character-unit wrap for justified alignment; hyphenation is bypassed here.
  if (characterWrap && blockStyle.alignment == CssTextAlign::Justify) {
    layoutCharacterWrap(renderer, fontId, rubyFontId, viewportWidth, spaceWidth, processLine, includeLastLine);
    return;
  }

  auto wordWidths = calculateWordWidths(renderer, fontId, rubyFontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, rubyFontId, pageWidth, wordWidths, wordContinues);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, rubyFontId, pageWidth, wordWidths, wordContinues);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, lineBreakIndices, processLine, renderer, fontId, rubyFontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId,
                                                      const int rubyFontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureTokenWidth(renderer, fontId, rubyFontId, words[i], rubyTexts[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int rubyFontId,
                                                  const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec) {
  if (words.empty()) {
    return {};
  }

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }
      if (j > static_cast<size_t>(i)) {
        gap += getRubyPairExtraGap(wordWidths[j - 1], measureRubyWidth(renderer, rubyFontId, rubyTexts[j - 1]),
                                   wordWidths[j], measureRubyWidth(renderer, rubyFontId, rubyTexts[j]), gap);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent() {
  if (!paragraphIndent || words.empty()) {
    return;
  }

  if (blockStyle.textIndentDefined) {
    // CSS text-indent is explicitly set (even if 0) - don't use fallback
    // The actual indent positioning is handled in extractLine()
  } else if (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left) {
    // No CSS text-indent defined - use ideographic space (U+3000) for Korean font compatibility
    words.front().insert(0, "\xe3\x80\x80");
  }
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int rubyFontId, const int pageWidth,
                                                            std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec) {
  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const int firstLineIndent =
      blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && !continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && continuesVec[currentIndex]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
      if (!isFirstWord) {
        spacing += getRubyPairExtraGap(wordWidths[currentIndex - 1],
                                       measureRubyWidth(renderer, rubyFontId, rubyTexts[currentIndex - 1]),
                                       wordWidths[currentIndex], measureRubyWidth(renderer, rubyFontId, rubyTexts[currentIndex]),
                                       spacing);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  if (!rubyTexts[wordIndex].empty()) {
    return false;
  }
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, std::string());
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId, const int rubyFontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate first line indent (only for left/justified text).
  // Positive text-indent (paragraph indent) is suppressed when extraParagraphSpacing is on.
  // Negative text-indent (hanging indent, e.g. margin-left:3em; text-indent:-1em) always applies —
  // it is structural (positions the bullet/marker), not decorative.
  const bool isFirstLine = breakIndex == 0;
  const int firstLineIndent =
      isFirstLine && blockStyle.textIndentDefined && (blockStyle.textIndent < 0 || !extraParagraphSpacing) &&
              (blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::Left)
          ? blockStyle.textIndent
          : 0;

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0) {
      int naturalGap = 0;
      if (!continuesVec[lastBreakAt + wordIdx]) {
        actualGapCount++;
        naturalGap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                                              firstCodepoint(words[lastBreakAt + wordIdx]),
                                              wordStyles[lastBreakAt + wordIdx - 1]);
      } else {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        naturalGap = renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx - 1]),
                                         firstCodepoint(words[lastBreakAt + wordIdx]),
                                         wordStyles[lastBreakAt + wordIdx - 1]);
      }
      totalNaturalGaps += naturalGap;
      totalNaturalGaps +=
          getRubyPairExtraGap(wordWidths[lastBreakAt + wordIdx - 1],
                              measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx - 1]),
                              wordWidths[lastBreakAt + wordIdx],
                              measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx]), naturalGap);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (blockStyle.alignment == CssTextAlign::Justify && !isLastLine && actualGapCount >= 1)
                               ? spareSpace / static_cast<int>(actualGapCount)
                               : 0;

  // Calculate initial x position (first line starts at indent for left/justified text;
  // may be negative for hanging indents, e.g. margin-left:3em; text-indent:-1em).
  auto xpos = static_cast<int16_t>(firstLineIndent);
  if (blockStyle.alignment == CssTextAlign::Right) {
    xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  } else if (blockStyle.alignment == CssTextAlign::Center) {
    xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
  }

  // Pre-calculate X positions for words
  // Continuation words attach to the previous word with no space before them
  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineXPos.push_back(xpos);

    const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
    if (nextIsContinuation) {
      int advance = wordWidths[lastBreakAt + wordIdx];
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      int gap = renderer.getKerning(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                    firstCodepoint(words[lastBreakAt + wordIdx + 1]), wordStyles[lastBreakAt + wordIdx]);
      gap += getRubyPairExtraGap(wordWidths[lastBreakAt + wordIdx],
                                 measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx]),
                                 wordWidths[lastBreakAt + wordIdx + 1],
                                 measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx + 1]), gap);
      advance += gap;
      xpos += advance;
    } else {
      int gap = 0;
      if (wordIdx + 1 < lineWordCount) {
        gap = renderer.getSpaceAdvance(fontId, lastCodepoint(words[lastBreakAt + wordIdx]),
                                       firstCodepoint(words[lastBreakAt + wordIdx + 1]),
                                       wordStyles[lastBreakAt + wordIdx]);
        if (blockStyle.alignment == CssTextAlign::Justify && !isLastLine) {
          gap += justifyExtra;
        }
        gap += getRubyPairExtraGap(wordWidths[lastBreakAt + wordIdx],
                                   measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx]),
                                   wordWidths[lastBreakAt + wordIdx + 1],
                                   measureRubyWidth(renderer, rubyFontId, rubyTexts[lastBreakAt + wordIdx + 1]), gap);
      }
      xpos += wordWidths[lastBreakAt + wordIdx] + gap;
    }
  }

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords(std::make_move_iterator(words.begin() + lastBreakAt),
                                     std::make_move_iterator(words.begin() + lineBreak));
  std::vector<std::string> lineRubyTexts(std::make_move_iterator(rubyTexts.begin() + lastBreakAt),
                                         std::make_move_iterator(rubyTexts.begin() + lineBreak));
  std::vector<EpdFontFamily::Style> lineWordStyles(wordStyles.begin() + lastBreakAt, wordStyles.begin() + lineBreak);
  std::vector<uint16_t> lineTokenWidths(wordWidths.begin() + lastBreakAt, wordWidths.begin() + lineBreak);

  for (auto& word : lineWords) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }

  for (size_t i = 0; i < lineWords.size(); ++i) {
    lineTokenWidths[i] =
        measureTokenWidth(renderer, fontId, rubyFontId, lineWords[i], lineRubyTexts[i], lineWordStyles[i]);
  }

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineRubyTexts), std::move(lineXPos),
                                          std::move(lineTokenWidths), std::move(lineWordStyles), blockStyle));
}
