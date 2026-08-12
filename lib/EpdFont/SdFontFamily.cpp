#include "SdFontFamily.h"

#include <algorithm>

#include <HardwareSerial.h>
#include <Utf8.h>

// ============================================================================
// SdFontFamily Implementation
// ============================================================================

SdFontFamily::SdFontFamily(const char* regularPath, const char* boldPath, const char* italicPath,
                           const char* boldItalicPath)
    : regular(nullptr), bold(nullptr), italic(nullptr), boldItalic(nullptr), ownsPointers(true) {
  if (regularPath) {
    regular = new SdFont(regularPath);
  }
  if (boldPath) {
    bold = new SdFont(boldPath);
  }
  if (italicPath) {
    italic = new SdFont(italicPath);
  }
  if (boldItalicPath) {
    boldItalic = new SdFont(boldItalicPath);
  }
}

SdFontFamily::~SdFontFamily() {
  if (ownsPointers) {
    delete regular;
    delete bold;
    delete italic;
    delete boldItalic;
  }
}

SdFontFamily::SdFontFamily(SdFontFamily&& other) noexcept
    : regular(other.regular),
      bold(other.bold),
      italic(other.italic),
      boldItalic(other.boldItalic),
      ownsPointers(other.ownsPointers) {
  other.regular = nullptr;
  other.bold = nullptr;
  other.italic = nullptr;
  other.boldItalic = nullptr;
  other.ownsPointers = false;
}

SdFontFamily& SdFontFamily::operator=(SdFontFamily&& other) noexcept {
  if (this != &other) {
    if (ownsPointers) {
      delete regular;
      delete bold;
      delete italic;
      delete boldItalic;
    }

    regular = other.regular;
    bold = other.bold;
    italic = other.italic;
    boldItalic = other.boldItalic;
    ownsPointers = other.ownsPointers;

    other.regular = nullptr;
    other.bold = nullptr;
    other.italic = nullptr;
    other.boldItalic = nullptr;
    other.ownsPointers = false;
  }
  return *this;
}

bool SdFontFamily::load() {
  bool success = true;

  if (regular && !regular->load()) {
    Serial.printf("[%lu] [SdFontFamily] Failed to load regular font\n", millis());
    success = false;
  }
  if (bold && !bold->load()) {
    Serial.printf("[%lu] [SdFontFamily] Failed to load bold font\n", millis());
    // Bold is optional, don't fail completely
  }
  if (italic && !italic->load()) {
    Serial.printf("[%lu] [SdFontFamily] Failed to load italic font\n", millis());
    // Italic is optional
  }
  if (boldItalic && !boldItalic->load()) {
    Serial.printf("[%lu] [SdFontFamily] Failed to load bold-italic font\n", millis());
    // Bold-italic is optional
  }

  return success;
}

bool SdFontFamily::isLoaded() const { return regular && regular->isLoaded(); }

SdFont* SdFontFamily::getFont(EpdFontStyle style) const {
  if (style == BOLD && bold && bold->isLoaded()) {
    return bold;
  }
  if (style == ITALIC && italic && italic->isLoaded()) {
    return italic;
  }
  if (style == BOLD_ITALIC) {
    if (boldItalic && boldItalic->isLoaded()) {
      return boldItalic;
    }
    if (bold && bold->isLoaded()) {
      return bold;
    }
    if (italic && italic->isLoaded()) {
      return italic;
    }
  }

  return regular;
}

void SdFontFamily::getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  if (font) {
    font->getTextDimensions(string, w, h);
  } else {
    *w = 0;
    *h = 0;
  }
}

bool SdFontFamily::hasPrintableChars(const char* string, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->hasPrintableChars(string) : false;
}

const EpdGlyph* SdFontFamily::getGlyph(uint32_t cp, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getGlyph(cp) : nullptr;
}

const uint8_t* SdFontFamily::getGlyphBitmap(uint32_t cp, EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getGlyphBitmap(cp) : nullptr;
}

uint8_t SdFontFamily::getAdvanceY(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getAdvanceY() : 0;
}

int8_t SdFontFamily::getAscender(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getAscender() : 0;
}

int8_t SdFontFamily::getDescender(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->getDescender() : 0;
}

bool SdFontFamily::is2Bit(EpdFontStyle style) const {
  SdFont* font = getFont(style);
  return font ? font->is2Bit() : false;
}

// ============================================================================
// UnifiedFontFamily Implementation
// ============================================================================

UnifiedFontFamily::UnifiedFontFamily(const EpdFontFamily* font)
    : type(Type::FLASH), flashFont(font), flashFallbackFont(nullptr), sdFont(nullptr) {}

UnifiedFontFamily::UnifiedFontFamily(const EpdFontFamily* font, const EpdFontFamily* fallbackFont)
    : type(Type::FLASH), flashFont(font), flashFallbackFont(fallbackFont), sdFont(nullptr) {}

UnifiedFontFamily::UnifiedFontFamily(SdFontFamily* font)
    : type(Type::SD), flashFont(nullptr), flashFallbackFont(nullptr), sdFont(font) {}

UnifiedFontFamily::~UnifiedFontFamily() {
  // flashFont is not owned (points to global), don't delete
  delete sdFont;
}

UnifiedFontFamily::UnifiedFontFamily(UnifiedFontFamily&& other) noexcept
    : type(other.type), flashFont(other.flashFont), flashFallbackFont(other.flashFallbackFont), sdFont(other.sdFont) {
  other.flashFont = nullptr;
  other.flashFallbackFont = nullptr;
  other.sdFont = nullptr;
}

UnifiedFontFamily& UnifiedFontFamily::operator=(UnifiedFontFamily&& other) noexcept {
  if (this != &other) {
    // flashFont is not owned (points to global), don't delete
    delete sdFont;

    type = other.type;
    flashFont = other.flashFont;
    flashFallbackFont = other.flashFallbackFont;
    sdFont = other.sdFont;

    other.flashFont = nullptr;
    other.flashFallbackFont = nullptr;
    other.sdFont = nullptr;
  }
  return *this;
}

void UnifiedFontFamily::getTextDimensions(const char* string, int* w, int* h, EpdFontStyle style) const {
  if (type == Type::SD && sdFont) {
    sdFont->getTextDimensions(string, w, h, style);
    return;
  }

  *w = 0;
  *h = 0;
  if (type != Type::FLASH || !flashFont || !string || *string == '\0') {
    return;
  }

  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  int lastBaseX = 0;
  int lastBaseLeft = 0;
  int lastBaseWidth = 0;
  int lastBaseTop = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t prevCp = 0;

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const bool isCombining = utf8IsCombiningMark(cp);
    if (!isCombining) {
      cp = applyLigatures(cp, string, style);
    }

    const EpdGlyph* glyph = lookupGlyph(cp, style).glyph;
    if (!glyph) {
      if (!isCombining) {
        lastBaseX += fp4::toPixel(prevAdvanceFP);
        prevCp = 0;
        prevAdvanceFP = 0;
        lastBaseLeft = 0;
        lastBaseWidth = 0;
        lastBaseTop = 0;
      }
      continue;
    }

    const int raiseBy = isCombining ? combiningMark::raiseAboveBase(glyph->top, glyph->height, lastBaseTop) : 0;

    if (!isCombining && prevCp != 0) {
      const auto kernFP = getKerning(prevCp, cp, style);
      lastBaseX += fp4::toPixel(prevAdvanceFP + kernFP);
    }

    const int glyphBaseX =
        isCombining ? combiningMark::centerOver(lastBaseX, lastBaseLeft, lastBaseWidth, glyph->left, glyph->width)
                    : lastBaseX;
    const int glyphBaseY = -raiseBy;

    minX = std::min(minX, glyphBaseX + glyph->left);
    maxX = std::max(maxX, glyphBaseX + glyph->left + glyph->width);
    minY = std::min(minY, glyphBaseY + glyph->top - glyph->height);
    maxY = std::max(maxY, glyphBaseY + glyph->top);

    if (!isCombining) {
      lastBaseLeft = glyph->left;
      lastBaseWidth = glyph->width;
      lastBaseTop = glyph->top;
      prevAdvanceFP = glyph->advanceX;
      prevCp = cp;
    }
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

bool UnifiedFontFamily::hasPrintableChars(const char* string, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->hasPrintableChars(string, style) ||
           (flashFallbackFont && flashFallbackFont->hasPrintableChars(string, style));
  } else if (sdFont) {
    return sdFont->hasPrintableChars(string, style);
  }
  return false;
}

UnifiedFontFamily::GlyphLookupResult UnifiedFontFamily::lookupGlyph(uint32_t cp, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* primaryData = flashFont->getData(style);
    if (const EpdGlyph* glyph = flashFont->getGlyphExact(cp, style)) {
      return {glyph, primaryData};
    }

    if (flashFallbackFont) {
      const EpdFontData* fallbackData = flashFallbackFont->getData(style);
      if (const EpdGlyph* glyph = flashFallbackFont->getGlyphExact(cp, style)) {
        return {glyph, fallbackData};
      }
    }

    return {flashFont->getGlyph(cp, style), primaryData};
  } else if (sdFont) {
    return {sdFont->getGlyph(cp, style), nullptr};
  }
  return {};
}

const EpdGlyph* UnifiedFontFamily::getGlyph(uint32_t cp, EpdFontStyle style) const {
  return lookupGlyph(cp, style).glyph;
}

const uint8_t* UnifiedFontFamily::getGlyphBitmap(uint32_t cp, EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const auto glyphLookup = lookupGlyph(cp, style);
    const EpdFontData* data = glyphLookup.data;
    const EpdGlyph* glyph = glyphLookup.glyph;
    if (data && glyph) {
      if (data->bitmapReader != nullptr) {
        return data->bitmapReader(data->bitmapReaderContext, glyph->dataOffset, glyph->dataLength);
      }
      return data->bitmap != nullptr ? &data->bitmap[glyph->dataOffset] : nullptr;
    }
    return nullptr;
  } else if (sdFont) {
    return sdFont->getGlyphBitmap(cp, style);
  }
  return nullptr;
}

uint8_t UnifiedFontFamily::getAdvanceY(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->advanceY : 0;
  } else if (sdFont) {
    return sdFont->getAdvanceY(style);
  }
  return 0;
}

int8_t UnifiedFontFamily::getAscender(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->ascender : 0;
  } else if (sdFont) {
    return sdFont->getAscender(style);
  }
  return 0;
}

int8_t UnifiedFontFamily::getDescender(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->descender : 0;
  } else if (sdFont) {
    return sdFont->getDescender(style);
  }
  return 0;
}

bool UnifiedFontFamily::is2Bit(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    const EpdFontData* data = flashFont->getData(style);
    return data ? data->is2Bit : false;
  } else if (sdFont) {
    return sdFont->is2Bit(style);
  }
  return false;
}

const EpdFontData* UnifiedFontFamily::getFlashData(EpdFontStyle style) const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->getData(style);
  }
  return nullptr;
}

const EpdFontData* UnifiedFontFamily::getFlashDataForCodepoint(uint32_t cp, EpdFontStyle style) const {
  return lookupGlyph(cp, style).data;
}

bool UnifiedFontFamily::hasBold() const {
  if (type == Type::FLASH && flashFont) {
    return flashFont->hasBold();
  } else if (sdFont) {
    return sdFont->hasBold();
  }
  return false;
}

int8_t UnifiedFontFamily::getKerning(uint32_t leftCp, uint32_t rightCp, EpdFontStyle style) const {
  // Only flash fonts carry kerning tables; SD fonts return 0 (no kerning).
  if (type == Type::FLASH && flashFont) {
    return flashFont->getKerning(leftCp, rightCp, style);
  }
  return 0;
}

uint32_t UnifiedFontFamily::applyLigatures(uint32_t cp, const char*& text, EpdFontStyle style) const {
  // Only flash fonts carry ligature tables; SD fonts pass codepoint through unchanged.
  if (type == Type::FLASH && flashFont) {
    return flashFont->applyLigatures(cp, text, style);
  }
  return cp;
}
