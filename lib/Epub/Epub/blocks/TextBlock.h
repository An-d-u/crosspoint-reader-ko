#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

struct RubyAnnotation {
  uint16_t startWordIndex;
  uint16_t wordCount;
  std::string text;
};

// Represents a line of text on a page
class TextBlock final : public Block {
 private:
  std::vector<std::string> words;
  std::vector<RubyAnnotation> rubyAnnotations;
  std::vector<int16_t> wordXpos;
  std::vector<uint16_t> tokenWidths;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<RubyAnnotation> rubyAnnotations,
                     std::vector<int16_t> word_xpos, std::vector<uint16_t> tokenWidths,
                     std::vector<EpdFontFamily::Style> word_styles,
                     const BlockStyle& blockStyle = BlockStyle())
      : words(std::move(words)),
        rubyAnnotations(std::move(rubyAnnotations)),
        wordXpos(std::move(word_xpos)),
        tokenWidths(std::move(tokenWidths)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle) {}
  ~TextBlock() override = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  bool hasRuby() const;
  int getRenderedLineHeight(const GfxRenderer& renderer, int fontId, int rubyFontId, float lineCompression,
                            bool verticalWritingMode = false) const;
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int rubyFontId, int x, int y,
              bool verticalWritingMode = false) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
