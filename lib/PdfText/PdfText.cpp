#include "PdfText.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <array>
#include <memory>
#include <new>

namespace PdfText {
namespace {
constexpr size_t BUFFER_SIZE = 1024;
constexpr uint32_t MAX_STREAM_SIZE = 4 * 1024 * 1024;
constexpr uint32_t MAX_DECODED_TOTAL = 32 * 1024 * 1024;

bool allocationRoom(size_t bytes) {
  return bytes < 256 * 1024 && esp_get_free_heap_size() > bytes + 16384 &&
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > bytes + 256;
}

struct Source {
  FsFile file;
  std::array<uint8_t, BUFFER_SIZE> buffer{};
  uint32_t size = 0, start = UINT32_MAX, count = 0;
  void close() {
    if (file.isOpen()) file.close();
    size = count = 0;
    start = UINT32_MAX;
  }
};

struct InflateContext {
  InflateReader reader;
  FsFile* input = nullptr;
  uint32_t remaining = 0;
  bool failed = false;
  std::array<uint8_t, BUFFER_SIZE> inputBuffer{};
  static int read(uzlib_uncomp* state) {
    auto* self = reinterpret_cast<InflateContext*>(state);
    if (!self->remaining) return -1;
    const size_t count = std::min<size_t>(self->remaining, self->inputBuffer.size());
    const int actual = self->input->read(self->inputBuffer.data(), count);
    if (actual != static_cast<int>(count)) { self->failed = true; return -1; }
    self->remaining -= count;
    state->source = self->inputBuffer.data() + 1;
    state->source_limit = self->inputBuffer.data() + count;
    return self->inputBuffer[0];
  }
};

class Backend {
  std::array<Source, 3> sources;
  std::array<uint8_t, BUFFER_SIZE> transfer{};
  const std::string& input;
  const std::string& directory;
  uint32_t decodedTotal = 0;
  unsigned long started = millis();

 public:
  Error error = Error::None;
  Backend(const std::string& input, const std::string& directory) : input(input), directory(directory) {}
  ~Backend() {
    for (auto& source : sources) source.close();
    Storage.remove((directory + "/pdf-content.tmp").c_str());
    Storage.remove((directory + "/pdf-cmap.tmp").c_str());
  }
  bool open() {
    if (!Storage.openFileForRead("PDF", input, sources[0].file)) { error = Error::Io; return false; }
    sources[0].size = sources[0].file.size();
    return true;
  }
  bool room(size_t bytes) const { return allocationRoom(bytes); }
  uint32_t size(unsigned source) const { return sources[source].size; }
  int byte(unsigned source, uint32_t offset) {
    auto& data = sources[source];
    if (error != Error::None || offset >= data.size) return -1;
    if (data.start == UINT32_MAX || offset < data.start || offset - data.start >= data.count) {
      if (millis() - started > 600000) { error = Error::Limit; return -1; }
      data.start = offset;
      data.count = std::min<uint32_t>(data.buffer.size(), data.size - offset);
      if (!data.file.seek(offset) || data.file.read(data.buffer.data(), data.count) != static_cast<int>(data.count)) {
        error = Error::Io; return -1;
      }
      vTaskDelay(1);
    }
    return data.buffer[offset - data.start];
  }
  bool decode(uint32_t offset, uint32_t length, bool compressed, unsigned slot) {
    sources[slot].close();
    const std::string path = directory + (slot == 1 ? "/pdf-content.tmp" : "/pdf-cmap.tmp");
    FsFile in, out;
    if (!Storage.openFileForRead("PDF", input, in) || !in.seek(offset) ||
        !Storage.openFileForWrite("PDF", path, out)) { error = Error::Io; return false; }
    uint32_t written = 0;
    uint32_t adlerA = 1, adlerB = 0;
    auto& buffer = transfer;
    auto write = [&](size_t count) {
      if (millis() - started > 600000) { error = Error::Limit; return false; }
      if (count > MAX_STREAM_SIZE - written || count > MAX_DECODED_TOTAL - decodedTotal) {
        error = Error::Limit; return false;
      }
      if (out.write(buffer.data(), count) != count) { error = Error::Io; return false; }
      written += count;
      decodedTotal += count;
      for (size_t i = 0; i < count; ++i) { adlerA = (adlerA + buffer[i]) % 65521; adlerB = (adlerB + adlerA) % 65521; }
      vTaskDelay(1);
      return true;
    };
    if (compressed) {
      if (length < 6) { error = Error::Invalid; return false; }
      const int cmf = byte(0, offset), flags = byte(0, offset + 1);
      if (cmf < 0 || flags < 0 || (cmf & 15) != 8 || (cmf >> 4) > 7 ||
          (cmf * 256 + flags) % 31 != 0 || (flags & 32)) { error = Error::Unsupported; return false; }
      if (!room(32768 + sizeof(InflateContext))) { error = Error::Memory; return false; }
      auto context = std::unique_ptr<InflateContext>(new (std::nothrow) InflateContext());
      if (!context || !context->reader.init(true)) { error = Error::Memory; return false; }
      context->input = &in;
      context->remaining = length;
      context->reader.setReadCallback(InflateContext::read);
      context->reader.skipZlibHeader();
      while (error == Error::None) {
        size_t count = 0;
        const auto status = context->reader.readAtMost(buffer.data(), buffer.size(), &count);
        if (status == InflateStatus::Error || context->failed) { error = Error::Invalid; break; }
        if (!write(count)) break;
        if (status == InflateStatus::Done) break;
        if (!count) { error = Error::Invalid; break; }
      }
      uint32_t expected = 0;
      for (unsigned i = 0; i < 4; ++i) {
        const int ch = byte(0, offset + length - 4 + i);
        if (ch < 0) break;
        expected = (expected << 8) | ch;
      }
      if (error == Error::None && expected != ((adlerB << 16) | adlerA)) error = Error::Invalid;
    } else {
      while (written < length && error == Error::None) {
        const size_t count = std::min<size_t>(buffer.size(), length - written);
        if (in.read(buffer.data(), count) != static_cast<int>(count)) { error = Error::Io; break; }
        if (!write(count)) break;
      }
    }
    const bool closed = out.close();
    if (!closed && error == Error::None) error = Error::Io;
    if (error != Error::None) return false;
    if (!Storage.openFileForRead("PDF", path, sources[slot].file)) { error = Error::Io; return false; }
    sources[slot].size = written;
    return true;
  }
};
}  // namespace

Error extract(const std::string& input, const std::string& output, const std::string& cacheDirectory) {
  Error result = Error::None;
  {
    if (!allocationRoom(sizeof(Backend) + 8192)) return Error::Memory;
    auto backend = std::unique_ptr<Backend>(new (std::nothrow) Backend(input, cacheDirectory));
    if (!backend) return Error::Memory;
    if (!backend->open()) return backend->error;
    FsFile text;
    if (!Storage.openFileForWrite("PDF", output, text)) return Error::Io;
    std::vector<char> buffer(BUFFER_SIZE);
    size_t used = 0;
    auto flush = [&]() {
      const bool ok = used == 0 || text.write(buffer.data(), used) == used;
      used = 0;
      return ok;
    };
    auto sink = [&](std::string_view value) {
      for (char ch : value) {
        buffer[used++] = ch;
        if (used == buffer.size() && !flush()) return false;
      }
      return true;
    };
    Parser parser(*backend, sink);
    result = parser.run();
    if (!flush() && result == Error::None) result = Error::Io;
    const bool closed = text.close();
    if (!closed && result == Error::None) result = Error::Io;
  }
  if (result != Error::None) Storage.remove(output.c_str());
  return result;
}
}  // namespace PdfText
