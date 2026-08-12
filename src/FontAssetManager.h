#pragma once

#include <EpdFontData.h>

#include <cstddef>
#include <cstdint>

class FontAssetManager {
 public:
  bool begin(EpdFontData& uiJapaneseFont, size_t expectedUiBitmapSize, EpdFontData& readerFont,
             size_t expectedReaderBitmapSize);
  bool isReady() const { return partition_ != nullptr; }

 private:
  static constexpr size_t CACHE_SLOT_SIZE = 4096;
  static constexpr size_t CACHE_SLOT_COUNT = 1;

  struct BitmapSource {
    FontAssetManager* owner = nullptr;
    uint32_t partitionOffset = 0;
    uint32_t size = 0;
    uint8_t id = 0;
  };

  struct CacheSlot {
    alignas(4) uint8_t data[CACHE_SLOT_SIZE]{};
    uint32_t sourceOffset = 0;
    uint16_t validSize = 0;
    uint8_t sourceId = 0;
    uint32_t lastUsed = 0;
    bool valid = false;
  };

  static const uint8_t* readBitmapCallback(void* context, uint32_t offset, uint16_t length);
  const uint8_t* readBitmap(const BitmapSource& source, uint32_t offset, uint16_t length);
  bool verifyChecksum(uint32_t partitionOffset, uint32_t size, uint32_t expectedChecksum);

  const void* partition_ = nullptr;
  BitmapSource uiSource_{};
  BitmapSource readerSource_{};
  CacheSlot cache_[CACHE_SLOT_COUNT]{};
  uint32_t cacheClock_ = 0;
};
