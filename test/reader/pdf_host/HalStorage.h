#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <cstdio>

// 호스트 검증에서만 사용하는 파일 어댑터. PDF 해석기·압축 해제 코드는 펌웨어 원본을 컴파일한다.
class FsFile {
  std::fstream file;
  size_t length = 0;
  size_t cursor = 0;
  bool writable = false;
 public:
  bool open(const std::string& path, bool write) {
    writable = write;
    file.open(path, std::ios::binary | (write ? std::ios::out | std::ios::trunc : std::ios::in));
    if (!file.is_open()) return false;
    if (!write) { file.seekg(0, std::ios::end); length = file.tellg(); file.seekg(0); }
    else length = 0;
    cursor = 0;
    return true;
  }
  bool isOpen() const { return file.is_open(); }
  size_t size() const { return length; }
  bool seek(size_t offset) {
    if (offset > length) return false;
    file.clear();
    if (writable) file.seekp(offset); else file.seekg(offset);
    cursor = offset;
    return bool(file);
  }
  int read(void* data, size_t count) {
    file.read(static_cast<char*>(data), count);
    const int actual = file.gcount();
    cursor += actual;
    return actual;
  }
  size_t write(const void* data, size_t count) {
    file.write(static_cast<const char*>(data), count);
    if (!file) return 0;
    cursor += count;
    length = std::max(length, cursor);
    return count;
  }
  bool close() {
    if (writable) file.flush();
    const bool result = !file.bad();
    file.close();
    return result;
  }
};
struct HostStorage {
  bool openFileForRead(const char*, const std::string& path, FsFile& file) { return file.open(path, false); }
  bool openFileForWrite(const char*, const std::string& path, FsFile& file) { return file.open(path, true); }
  bool remove(const char* path) { return std::remove(path) == 0; }
};
inline HostStorage Storage;
inline unsigned long millis() { return 0; }
inline void vTaskDelay(int) {}
