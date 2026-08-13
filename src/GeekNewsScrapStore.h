#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GeekNewsScrap {
  int32_t topicId = 0;
  std::string title;
  std::string url;
};

class GeekNewsScrapStore {
 public:
  enum class AddResult { Added, AlreadyExists, Failed };

  bool load();
  AddResult add(GeekNewsScrap scrap);
  bool removeAt(size_t index);
  int findIndex(int32_t topicId, const std::string& url) const;
  static std::string topicUrl(int32_t topicId);

  const std::vector<GeekNewsScrap>& getScraps() const { return scraps_; }

 private:
  static constexpr size_t kMaxScraps = 20;
  static constexpr size_t kMaxTitleBytes = 192;
  static constexpr size_t kMaxUrlBytes = 512;

  std::vector<GeekNewsScrap> scraps_;

  bool loadFromFile(const char* path);
  bool save() const;
  bool normalizeUrls();
  static void truncateUtf8(std::string& value, size_t maxBytes);
};
