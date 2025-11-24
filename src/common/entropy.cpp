#include "common/entropy.h"
#include <cmath>

constexpr unsigned int ILP = 4;

struct alignas(64) AlignedHistogram {
  unsigned int data[256];
};

std::array<unsigned int, 256> count_symbols(std::span<const uint8_t> input) {
  std::array<AlignedHistogram, ILP> streams;
  for (auto &s : streams)
    std::fill(std::begin(s.data), std::end(s.data), 0);

  const uint8_t *ptr = input.data();
  size_t len = input.size();

  while (len > 0 && (reinterpret_cast<uintptr_t>(ptr) & 7)) {
    streams[0].data[*ptr]++;
    ptr++;
    len--;
  }

  auto run_batch = [&](auto seq) {
    [ ptr, &streams ]<size_t... Is>(std::index_sequence<Is...>)
        __attribute__((always_inline)) {
      ((streams[Is].data[ptr[Is]]++), ...);
    }
    (seq);
  };

  while (len >= ILP) {
    run_batch(std::make_index_sequence<ILP>{});
    ptr += ILP;
    len -= ILP;
  }

  while (len > 0) {
    streams[0].data[*ptr]++;
    ptr++;
    len--;
  }

  std::array<unsigned int, 256> ans = {};
  for (size_t s = 0; s < 256; ++s) {
    unsigned int sum = 0;
    for (size_t i = 0; i < ILP; ++i)
      sum += streams[i].data[s];
    ans[s] = sum;
  }
  return ans;
}

double get_entropy(std::span<const uint8_t> input) {
  auto counts = count_symbols(input);
  double entropy = 0.0;
  for (uint64_t c : counts) {
    if (c > 0) {
      double p = static_cast<double>(c) / input.size();
      entropy -= p * std::log2(p);
    }
  }
  return entropy;
}