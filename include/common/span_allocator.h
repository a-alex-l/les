#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

template <typename T>
std::span<T> alloc_span(std::span<uint8_t> &memory, size_t count) {
  if (count == 0)
    return {};

  void *ptr = memory.data();
  size_t space = memory.size();

  // std::align modifies 'ptr' to point to the aligned address
  // and decreases 'space' by the amount of padding used.
  void *aligned = std::align(64, count * sizeof(T), ptr, space);

  if (!aligned)
    throw std::bad_alloc();

  size_t padding = static_cast<uint8_t *>(aligned) - memory.data();
  size_t total_consumed = padding + count * sizeof(T);

  memory = memory.subspan(total_consumed);
  return std::span<T>(static_cast<T *>(aligned), count);
}
