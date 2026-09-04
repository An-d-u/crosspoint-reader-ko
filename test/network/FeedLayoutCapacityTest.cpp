// 실제 용량 확장 함수를 컴파일 시점에 평가한다. 데이터는 합성 값만 사용한다.
#include "activities/news/FeedLayoutCapacity.h"

using namespace FeedLayoutCapacity;

constexpr bool growPastOldLimits() {
  std::vector<uint32_t> lines;
  std::vector<uint16_t> spans;
  std::vector<char> text;
  auto room = [](size_t bytes) { return bytes <= 32 * 1024; };
  for (size_t i = 0; i < 1300; ++i) {
    if (ensure(lines, i + 1, 8192, 128, room).status != Status::Ready ||
        ensure(spans, i * 2 + 2, 8192, 256, room).status != Status::Ready ||
        ensure(text, i * 2 + 2, UINT16_MAX, 4096, room).status != Status::Ready) return false;
    if (lines.capacity() >= i + 1 + 128 || spans.capacity() >= i * 2 + 2 + 256) return false;
    lines.push_back(static_cast<uint32_t>(i));
    spans.push_back(static_cast<uint16_t>(i));
    spans.push_back(static_cast<uint16_t>(i + 1));
    text.push_back('a');
    text.push_back('\0');
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i] != i || spans[i * 2] != i || spans[i * 2 + 1] != i + 1 ||
        text[i * 2] != 'a' || text[i * 2 + 1] != '\0') return false;
  }
  return lines.size() == 1300 && spans.size() == 2600 && lines.capacity() < 8192;
}
static_assert(growPastOldLimits(), "512줄·1024구간 초과 확장 또는 기존 내용 보존 실패");

constexpr bool denyFullReplacement() {
  std::vector<uint32_t> values;
  values.reserve(128);
  values.push_back(42);
  size_t requested = 0;
  // 증가분 512바이트가 아닌 새 버퍼 전체 1024바이트를 검사해야 한다.
  const auto failed = ensure(values, 129, 8192, 128, [&](size_t bytes) {
    requested = bytes;
    return bytes <= 700;
  });
  if (failed.status != Status::Memory || failed.allocationBytes != 1024 || requested != 1024 ||
      values.capacity() != 128 || values.size() != 1 || values[0] != 42) return false;
  // 이미 확보한 용량은 여유 메모리가 없어도 그대로 사용한다.
  return ensure(values, 128, 8192, 128, [](size_t) { return false; }).status == Status::Ready;
}
static_assert(denyFullReplacement(), "메모리 부족 시 원본 버퍼 변경 또는 할당량 검사 오류");

constexpr bool limitsAndRounding() {
  std::vector<char> bytes;
  if (ensure(bytes, 1, 1000, 128, [](size_t n) { return n == 128; }).status != Status::Ready) return false;
  if (ensure(bytes, 999, 1000, 128, [](size_t n) { return n == 1000; }).status != Status::Ready) return false;
  bool called = false;
  if (ensure(bytes, 1001, 1000, 128, [&](size_t) { called = true; return true; }).status != Status::Limit || called) return false;
  std::vector<uint32_t> huge;
  if (ensure(huge, SIZE_MAX, SIZE_MAX, 128, [&](size_t) { called = true; return true; }).status != Status::Limit || called) return false;
  std::vector<char> empty;
  return ensure(empty, 0, 1000, 128, [](size_t) { return false; }).status == Status::Ready && empty.capacity() == 0;
}
static_assert(limitsAndRounding(), "작은 초기 할당·상한·정수 넘침·빈 버퍼 경계 실패");
