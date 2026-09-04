#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace FeedLayoutCapacity {
enum class Status { Ready, Limit, Memory };
struct Result {
  Status status;
  size_t allocationBytes = 0;
};

// 기존 버퍼가 살아 있는 동안 새 버퍼 전체가 필요하므로, 증가분이 아닌 전체 할당량을 검사한다.
template <typename T, typename Check>
constexpr Result ensure(std::vector<T>& storage, const size_t required, const size_t limit,
                         const size_t step, Check canAllocate) {
  if (required > limit) return {Status::Limit};
  if (required <= storage.capacity()) return {Status::Ready};
  size_t target = required;
  if (step > 0 && required % step != 0) {
    const size_t extra = step - required % step;
    target += extra < limit - required ? extra : limit - required;
  }
  if (target > SIZE_MAX / sizeof(T)) return {Status::Limit};
  const size_t bytes = target * sizeof(T);
  if (!canAllocate(bytes)) return {Status::Memory, bytes};
  storage.reserve(target);
  return {Status::Ready};
}
}  // namespace FeedLayoutCapacity
