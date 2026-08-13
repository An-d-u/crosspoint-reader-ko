#include "GeekNewsScrapStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <utility>

namespace {
constexpr uint8_t kFileVersion = 1;
constexpr char kDirectory[] = "/.crosspoint";
constexpr char kStoragePath[] = "/.crosspoint/geeknews_scraps.bin";
constexpr char kTemporaryPath[] = "/.crosspoint/geeknews_scraps.tmp";
constexpr char kBackupPath[] = "/.crosspoint/geeknews_scraps.bak";
constexpr size_t kMaxStoredScraps = 20;
constexpr size_t kMaxStoredTitleBytes = 192;
constexpr size_t kMaxStoredUrlBytes = 512;
constexpr size_t kMaxFileBytes = 2 + kMaxStoredScraps * (sizeof(int32_t) + sizeof(uint16_t) * 2 +
                                                         kMaxStoredTitleBytes + kMaxStoredUrlBytes);

bool readExact(HalFile& file, void* output, size_t bytes) {
  auto* cursor = static_cast<uint8_t*>(output);
  while (bytes > 0) {
    const int count = file.read(cursor, bytes);
    if (count <= 0) return false;
    cursor += count;
    bytes -= static_cast<size_t>(count);
  }
  return true;
}

bool writeExact(HalFile& file, const void* input, const size_t bytes) {
  auto* cursor = static_cast<const uint8_t*>(input);
  size_t remaining = bytes;
  while (remaining > 0) {
    const size_t count = file.write(cursor, remaining);
    if (count == 0) return false;
    cursor += count;
    remaining -= count;
  }
  return true;
}

template <typename T>
bool readValue(HalFile& file, T& value) {
  return readExact(file, &value, sizeof(value));
}

template <typename T>
bool writeValue(HalFile& file, const T& value) {
  return writeExact(file, &value, sizeof(value));
}

bool readString(HalFile& file, std::string& value, const size_t maxBytes) {
  uint16_t length = 0;
  if (!readValue(file, length) || length > maxBytes) return false;
  value.resize(length);
  return length == 0 || readExact(file, value.data(), length);
}

bool writeString(HalFile& file, const std::string& value) {
  const uint16_t length = static_cast<uint16_t>(value.size());
  return writeValue(file, length) && (length == 0 || writeExact(file, value.data(), length));
}
}  // namespace

bool GeekNewsScrapStore::load() {
  scraps_.clear();
  const bool primaryExists = Storage.exists(kStoragePath);
  if (primaryExists && loadFromFile(kStoragePath)) {
    return !normalizeUrls() || save();
  }
  scraps_.clear();
  if (!Storage.exists(kBackupPath)) return !primaryExists;
  if (!loadFromFile(kBackupPath)) return false;

  Storage.remove(kStoragePath);
  if (!Storage.rename(kBackupPath, kStoragePath)) return false;
  return !normalizeUrls() || save();
}

bool GeekNewsScrapStore::loadFromFile(const char* path) {
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file || file.fileSize() > kMaxFileBytes) return false;

  uint8_t version = 0;
  uint8_t count = 0;
  if (!readValue(file, version) || !readValue(file, count) || version != kFileVersion || count > kMaxScraps) {
    return false;
  }

  std::vector<GeekNewsScrap> loaded;
  loaded.reserve(count);
  for (uint8_t index = 0; index < count; ++index) {
    GeekNewsScrap scrap;
    if (!readValue(file, scrap.topicId) || !readString(file, scrap.title, kMaxTitleBytes) ||
        !readString(file, scrap.url, kMaxUrlBytes) || scrap.topicId <= 0 || scrap.title.empty() || scrap.url.empty()) {
      return false;
    }
    loaded.push_back(std::move(scrap));
  }

  scraps_ = std::move(loaded);
  return true;
}

GeekNewsScrapStore::AddResult GeekNewsScrapStore::add(GeekNewsScrap scrap) {
  if (scrap.topicId <= 0 || scrap.title.empty()) return AddResult::Failed;
  scrap.url = topicUrl(scrap.topicId);
  if (findIndex(scrap.topicId, scrap.url) >= 0) return AddResult::AlreadyExists;
  if (scraps_.size() >= kMaxScraps) return AddResult::Failed;

  truncateUtf8(scrap.title, kMaxTitleBytes);
  truncateUtf8(scrap.url, kMaxUrlBytes);
  scraps_.insert(scraps_.begin(), std::move(scrap));
  if (save()) return AddResult::Added;
  scraps_.erase(scraps_.begin());
  return AddResult::Failed;
}

bool GeekNewsScrapStore::removeAt(const size_t index) {
  if (index >= scraps_.size()) return false;
  GeekNewsScrap removed = std::move(scraps_[index]);
  scraps_.erase(scraps_.begin() + index);
  if (save()) return true;
  scraps_.insert(scraps_.begin() + index, std::move(removed));
  return false;
}

int GeekNewsScrapStore::findIndex(const int32_t topicId, const std::string& url) const {
  for (size_t index = 0; index < scraps_.size(); ++index) {
    if (scraps_[index].topicId == topicId || (!url.empty() && scraps_[index].url == url)) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

std::string GeekNewsScrapStore::topicUrl(const int32_t topicId) {
  return std::string("https://news.hada.io/topic?id=") + std::to_string(topicId);
}

bool GeekNewsScrapStore::save() const {
  Storage.mkdir(kDirectory);
  HalFile file = Storage.open(kTemporaryPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return false;

  const uint8_t count = static_cast<uint8_t>(scraps_.size());
  bool saved = writeValue(file, kFileVersion) && writeValue(file, count);
  for (const GeekNewsScrap& scrap : scraps_) {
    saved = saved && writeValue(file, scrap.topicId) && writeString(file, scrap.title) && writeString(file, scrap.url);
  }
  file.flush();
  saved = file.close() && saved;
  if (!saved) {
    Storage.remove(kTemporaryPath);
    return false;
  }

  Storage.remove(kBackupPath);
  const bool hadOriginal = Storage.exists(kStoragePath);
  if (hadOriginal && !Storage.rename(kStoragePath, kBackupPath)) {
    Storage.remove(kTemporaryPath);
    return false;
  }
  if (!Storage.rename(kTemporaryPath, kStoragePath)) {
    if (hadOriginal) Storage.rename(kBackupPath, kStoragePath);
    Storage.remove(kTemporaryPath);
    return false;
  }
  if (hadOriginal) Storage.remove(kBackupPath);
  return true;
}

bool GeekNewsScrapStore::normalizeUrls() {
  bool changed = false;
  for (GeekNewsScrap& scrap : scraps_) {
    const std::string expected = topicUrl(scrap.topicId);
    if (scrap.url != expected) {
      scrap.url = expected;
      changed = true;
    }
  }
  return changed;
}

void GeekNewsScrapStore::truncateUtf8(std::string& value, const size_t maxBytes) {
  if (value.size() <= maxBytes) return;
  size_t safeSize = maxBytes;
  while (safeSize > 0 && (static_cast<unsigned char>(value[safeSize]) & 0xC0) == 0x80) --safeSize;
  value.resize(safeSize);
}
