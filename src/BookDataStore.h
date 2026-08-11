#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct BookDataReference {
  std::string id;
  std::string cacheKey;
  std::string previousPath;
  std::vector<std::string> knownPaths;
  uint32_t fileSize = 0;
};

namespace BookDataStore {
BookDataReference resolve(const std::string& path);
std::string legacyCacheKey(const std::string& path);
}  // namespace BookDataStore
