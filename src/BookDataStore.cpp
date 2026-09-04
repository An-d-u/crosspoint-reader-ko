#include "BookDataStore.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <functional>
#include <utility>

namespace {
constexpr char registryPath[] = "/.crosspoint/book-data.json";
constexpr int registryVersion = 1;
constexpr size_t maxEntries = 50;
constexpr size_t maxAliases = 4;
constexpr size_t sampleSize = 4096;
constexpr size_t readBufferSize = 512;
constexpr uint64_t fnvOffset = 14695981039346656037ULL;
constexpr uint64_t fnvPrime = 1099511628211ULL;

struct RegistryEntry {
  std::string id;
  std::string cacheKey;
  std::string path;
  std::vector<std::string> aliases;
  uint32_t fileSize = 0;
};

std::vector<RegistryEntry> entries;
bool registryLoaded = false;
bool registryWritable = true;

void hashBytes(uint64_t& hash, const void* data, const size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= fnvPrime;
  }
}

bool hashRange(FsFile& file, const size_t offset, const size_t length, uint64_t& hash) {
  if (!file.seekSet(offset)) {
    return false;
  }

  std::array<uint8_t, readBufferSize> buffer{};
  size_t remaining = length;
  while (remaining > 0) {
    const size_t requested = std::min(remaining, buffer.size());
    const int bytesRead = file.read(buffer.data(), requested);
    if (bytesRead <= 0) {
      return false;
    }
    hashBytes(hash, buffer.data(), static_cast<size_t>(bytesRead));
    remaining -= static_cast<size_t>(bytesRead);
  }
  return true;
}

bool fingerprint(const std::string& path, std::string& id, uint32_t& fileSize) {
  FsFile file;
  if (!Storage.openFileForRead("BDS", path, file) || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  const size_t size = file.size();
  if (size > UINT32_MAX) {
    file.close();
    return false;
  }

  uint64_t hash = fnvOffset;
  const uint8_t fingerprintVersion = 1;
  const uint32_t contentSize = static_cast<uint32_t>(size);
  hashBytes(hash, &fingerprintVersion, sizeof(fingerprintVersion));
  hashBytes(hash, &contentSize, sizeof(contentSize));

  bool success = true;
  if (size <= sampleSize * 3) {
    success = hashRange(file, 0, size, hash);
  } else {
    const std::array<size_t, 3> offsets = {0, (size - sampleSize) / 2, size - sampleSize};
    for (const size_t offset : offsets) {
      const uint32_t sampleOffset = static_cast<uint32_t>(offset);
      hashBytes(hash, &sampleOffset, sizeof(sampleOffset));
      if (!hashRange(file, offset, sampleSize, hash)) {
        success = false;
        break;
      }
    }
  }
  file.close();

  if (!success) {
    return false;
  }

  char encoded[17];
  snprintf(encoded, sizeof(encoded), "%016llx", static_cast<unsigned long long>(hash));
  id = encoded;
  fileSize = contentSize;
  return true;
}

bool containsPath(const RegistryEntry& entry, const std::string& path) {
  return entry.path == path || std::find(entry.aliases.begin(), entry.aliases.end(), path) != entry.aliases.end();
}

bool validId(const std::string& id) {
  return id.size() == 16 && std::all_of(id.begin(), id.end(), [](const unsigned char character) {
           return std::isxdigit(character) != 0;
         });
}

bool validCacheKey(const std::string& cacheKey) {
  return !cacheKey.empty() && cacheKey.size() <= 32 &&
         std::all_of(cacheKey.begin(), cacheKey.end(), [](const unsigned char character) {
           return std::isalnum(character) != 0 || character == '_';
         });
}

void addAlias(RegistryEntry& entry, const std::string& path) {
  if (path.empty() || path == entry.path ||
      std::find(entry.aliases.begin(), entry.aliases.end(), path) != entry.aliases.end()) {
    return;
  }
  if (entry.aliases.size() >= maxAliases) {
    entry.aliases.erase(entry.aliases.begin());
  }
  entry.aliases.push_back(path);
}

std::vector<std::string> collectPaths(const RegistryEntry& entry) {
  std::vector<std::string> paths;
  paths.reserve(entry.aliases.size() + 1);
  paths.push_back(entry.path);
  for (const std::string& alias : entry.aliases) {
    if (alias != entry.path) {
      paths.push_back(alias);
    }
  }
  return paths;
}

bool loadRegistry() {
  if (registryLoaded) {
    return registryWritable;
  }
  registryLoaded = true;
  entries.clear();

  if (!Storage.exists(registryPath)) {
    return true;
  }

  const String json = Storage.readFile(registryPath);
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, json);
  if (error || (document["version"] | 0) != registryVersion) {
    LOG_ERR("BDS", "Book data registry is invalid");
    registryWritable = false;
    return false;
  }

  const JsonArrayConst books = document["books"].as<JsonArrayConst>();
  entries.reserve(std::min(books.size(), maxEntries));
  for (const JsonObjectConst item : books) {
    if (entries.size() >= maxEntries) {
      break;
    }

    RegistryEntry entry;
    entry.id = item["id"] | std::string();
    entry.cacheKey = item["cacheKey"] | std::string();
    entry.path = item["path"] | std::string();
    entry.fileSize = item["fileSize"] | 0U;
    if (!validId(entry.id) || !validCacheKey(entry.cacheKey) || entry.path.empty()) {
      continue;
    }

    for (const JsonVariantConst alias : item["aliases"].as<JsonArrayConst>()) {
      addAlias(entry, alias | std::string());
    }
    entries.push_back(std::move(entry));
  }
  return true;
}

bool saveRegistry() {
  if (!registryWritable) {
    return false;
  }

  Storage.mkdir("/.crosspoint");
  JsonDocument document;
  document["version"] = registryVersion;
  JsonArray books = document["books"].to<JsonArray>();
  for (const RegistryEntry& entry : entries) {
    JsonObject item = books.add<JsonObject>();
    item["id"] = entry.id;
    item["cacheKey"] = entry.cacheKey;
    item["path"] = entry.path;
    item["fileSize"] = entry.fileSize;
    JsonArray aliases = item["aliases"].to<JsonArray>();
    for (const std::string& alias : entry.aliases) {
      aliases.add(alias);
    }
  }

  String json;
  serializeJson(document, json);
  return Storage.writeFile(registryPath, json);
}

const char* cachePrefix(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return "epub";
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return "xtc";
  }
  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path) || FsHelpers::hasPdfExtension(path)) {
    return "txt";
  }
  return nullptr;
}

bool cacheExists(const std::string& path, const std::string& cacheKey) {
  const char* prefix = cachePrefix(path);
  if (!prefix) {
    return false;
  }
  const std::string cachePath = std::string("/.crosspoint/") + prefix + "_" + cacheKey;
  return Storage.exists(cachePath.c_str());
}
}  // namespace

std::string BookDataStore::legacyCacheKey(const std::string& path) {
  return std::to_string(std::hash<std::string>{}(path));
}

BookDataReference BookDataStore::resolve(const std::string& path) {
  BookDataReference reference;
  reference.cacheKey = legacyCacheKey(path);
  if (path.empty() || !fingerprint(path, reference.id, reference.fileSize)) {
    reference.knownPaths.push_back(path);
    return reference;
  }

  const bool canSave = loadRegistry();
  auto entry = std::find_if(entries.begin(), entries.end(), [&](const RegistryEntry& item) {
    return item.id == reference.id && item.fileSize == reference.fileSize;
  });
  if (entry != entries.end()) {
    bool changed = false;
    if (entry->path != path) {
      std::string currentPathId;
      uint32_t currentPathSize = 0;
      const bool currentPathMatches = fingerprint(entry->path, currentPathId, currentPathSize) &&
                                      currentPathId == entry->id && currentPathSize == entry->fileSize;
      if (!currentPathMatches) {
        reference.previousPath = entry->path;
        const std::string previousPath = entry->path;
        entry->path = path;
        addAlias(*entry, previousPath);
        changed = true;
      } else if (!containsPath(*entry, path)) {
        addAlias(*entry, path);
        changed = true;
      }
    }
    if (changed && canSave) {
      saveRegistry();
    }
    reference.cacheKey = entry->cacheKey;
    reference.knownPaths = collectPaths(*entry);
    return reference;
  }

  bool pathBelongsToAnotherBook = false;
  for (const RegistryEntry& item : entries) {
    if (containsPath(item, path)) {
      pathBelongsToAnotherBook = true;
      break;
    }
  }

  const std::string oldCacheKey = legacyCacheKey(path);
  reference.cacheKey = !pathBelongsToAnotherBook && cacheExists(path, oldCacheKey) ? oldCacheKey
                                                                                  : "id_" + reference.id;
  reference.knownPaths.push_back(path);
  if (entries.size() < maxEntries && canSave) {
    entries.push_back({reference.id, reference.cacheKey, path, {}, reference.fileSize});
    saveRegistry();
  }
  return reference;
}
