#pragma once

#include <cstddef>
#include <cstdint>

namespace TextReaderCache {
constexpr uint32_t MAGIC = 0x54585449;
constexpr uint32_t VERSION = 5;
constexpr size_t HEADER_WORDS = 16;
constexpr size_t HEADER_BYTES = HEADER_WORDS * sizeof(uint32_t);

constexpr uint32_t read32(const uint8_t* data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

// 아직 다시 열지 않은 TXT의 구형 색인도 최근 도서 진행률에 사용할 수 있다.
constexpr uint32_t pageCount(const uint8_t* data, size_t bytes, uint32_t sourceSize) {
  if (bytes < 35 || read32(data) != MAGIC) return 0;
  if (data[4] == 4 && read32(data + 5) == sourceSize) return read32(data + 31);
  if (bytes >= HEADER_BYTES && read32(data + 4) == VERSION && read32(data + 8) == sourceSize) {
    const auto count = read32(data + 60);
    return count <= 65535 ? count : 0;
  }
  return 0;
}
}  // namespace TextReaderCache
