#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_8BIT = 0;
inline size_t heap_caps_get_largest_free_block(int) { return 16 * 1024 * 1024; }
