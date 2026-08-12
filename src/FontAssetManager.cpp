#include "FontAssetManager.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_err.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdint>

namespace {

constexpr uint32_t ASSET_MAGIC = 0x53415043;  // "CPAS" 리틀 엔디언
constexpr uint16_t ASSET_FORMAT_VERSION = 1;
constexpr uint8_t ASSET_PARTITION_SUBTYPE = 0x40;
constexpr char ASSET_PARTITION_LABEL[] = "assets";

struct __attribute__((packed)) AssetRecord {
  uint32_t offset;
  uint32_t size;
  uint32_t checksum;
};

struct __attribute__((packed)) AssetHeader {
  uint32_t magic;
  uint16_t formatVersion;
  uint16_t headerSize;
  uint32_t totalSize;
  uint32_t payloadChecksum;
  AssetRecord uiJapanese;
  AssetRecord reader;
  uint32_t reserved[6];
};

static_assert(sizeof(AssetHeader) == 64, "글꼴 assets 헤더 크기는 64바이트여야 합니다");

bool recordIsValid(const AssetRecord& record, const AssetHeader& header, size_t expectedSize) {
  if (record.size != expectedSize || record.offset < header.headerSize || record.offset > header.totalSize) {
    return false;
  }
  return record.size <= header.totalSize - record.offset;
}

}  // namespace

bool FontAssetManager::begin(EpdFontData& uiJapaneseFont, const size_t expectedUiBitmapSize,
                             EpdFontData& readerFont, const size_t expectedReaderBitmapSize) {
  uiJapaneseFont.bitmap = nullptr;
  uiJapaneseFont.bitmapReader = nullptr;
  uiJapaneseFont.bitmapReaderContext = nullptr;
  readerFont.bitmap = nullptr;
  readerFont.bitmapReader = nullptr;
  readerFont.bitmapReaderContext = nullptr;

  const auto subtype = static_cast<esp_partition_subtype_t>(ASSET_PARTITION_SUBTYPE);
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, ASSET_PARTITION_LABEL);
  if (partition == nullptr) {
    LOG_ERR("FNT", "Font assets partition not found");
    return false;
  }

  AssetHeader header{};
  const esp_err_t readResult = esp_partition_read(partition, 0, &header, sizeof(header));
  if (readResult != ESP_OK) {
    LOG_ERR("FNT", "Font assets header read failed: %s", esp_err_to_name(readResult));
    return false;
  }

  if (header.magic != ASSET_MAGIC || header.formatVersion != ASSET_FORMAT_VERSION ||
      header.headerSize != sizeof(AssetHeader) || header.totalSize > partition->size ||
      !recordIsValid(header.uiJapanese, header, expectedUiBitmapSize) ||
      !recordIsValid(header.reader, header, expectedReaderBitmapSize)) {
    LOG_ERR("FNT", "Font assets header is invalid or incompatible");
    return false;
  }

  partition_ = partition;
  if (!verifyChecksum(header.uiJapanese.offset, header.uiJapanese.size, header.uiJapanese.checksum) ||
      !verifyChecksum(header.reader.offset, header.reader.size, header.reader.checksum)) {
    LOG_ERR("FNT", "Font assets checksum mismatch");
    partition_ = nullptr;
    return false;
  }

  uiSource_ = {this, header.uiJapanese.offset, header.uiJapanese.size, 1};
  readerSource_ = {this, header.reader.offset, header.reader.size, 2};
  uiJapaneseFont.bitmapReader = &FontAssetManager::readBitmapCallback;
  uiJapaneseFont.bitmapReaderContext = &uiSource_;
  readerFont.bitmapReader = &FontAssetManager::readBitmapCallback;
  readerFont.bitmapReaderContext = &readerSource_;
  LOG_INF("FNT", "Font assets ready with %u-byte cache", static_cast<unsigned>(sizeof(cache_)));
  return true;
}

const uint8_t* FontAssetManager::readBitmapCallback(void* context, const uint32_t offset, const uint16_t length) {
  if (context == nullptr) {
    return nullptr;
  }
  const auto& source = *static_cast<const BitmapSource*>(context);
  return source.owner != nullptr ? source.owner->readBitmap(source, offset, length) : nullptr;
}

const uint8_t* FontAssetManager::readBitmap(const BitmapSource& source, const uint32_t offset,
                                            const uint16_t length) {
  if (partition_ == nullptr || length == 0 || length > CACHE_SLOT_SIZE || offset > source.size ||
      length > source.size - offset) {
    return nullptr;
  }

  ++cacheClock_;
  for (auto& slot : cache_) {
    if (slot.valid && slot.sourceId == source.id && offset >= slot.sourceOffset &&
        offset + length <= slot.sourceOffset + slot.validSize) {
      slot.lastUsed = cacheClock_;
      return slot.data + (offset - slot.sourceOffset);
    }
  }

  CacheSlot* target = &cache_[0];
  for (auto& slot : cache_) {
    if (!slot.valid) {
      target = &slot;
      break;
    }
    if (slot.lastUsed < target->lastUsed) {
      target = &slot;
    }
  }

  const uint32_t readSize = static_cast<uint32_t>(std::min<size_t>(CACHE_SLOT_SIZE, source.size - offset));
  const auto* partition = static_cast<const esp_partition_t*>(partition_);
  const esp_err_t result = esp_partition_read(partition, source.partitionOffset + offset, target->data, readSize);
  if (result != ESP_OK) {
    target->valid = false;
    LOG_ERR("FNT", "Font bitmap read failed: %s", esp_err_to_name(result));
    return nullptr;
  }

  target->sourceOffset = offset;
  target->validSize = static_cast<uint16_t>(readSize);
  target->sourceId = source.id;
  target->lastUsed = cacheClock_;
  target->valid = true;
  return target->data;
}

bool FontAssetManager::verifyChecksum(const uint32_t partitionOffset, const uint32_t size,
                                      const uint32_t expectedChecksum) {
  auto* buffer = cache_[0].data;
  uint32_t checksum = 0x811C9DC5;
  uint32_t consumed = 0;
  const auto* partition = static_cast<const esp_partition_t*>(partition_);
  while (consumed < size) {
    const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(CACHE_SLOT_SIZE, size - consumed));
    const esp_err_t result = esp_partition_read(partition, partitionOffset + consumed, buffer, chunk);
    if (result != ESP_OK) {
      return false;
    }
    for (uint32_t index = 0; index < chunk; ++index) {
      checksum ^= buffer[index];
      checksum *= 0x01000193;
    }
    consumed += chunk;
    delay(0);
  }
  cache_[0].valid = false;
  return checksum == expectedChecksum;
}
