#include "compression_classifier.h"
#include "common/entropy.h"
#include <algorithm>
#include <cmath>
#include <vector>

std::array<CompressorType, 5>
CompressionClassifier::get_best_candidates(std::span<const uint8_t> input,
                                           std::span<uint8_t> delta_buffer) {
  if (input.empty())
    return {CompressorType::NONE};

  uint8_t prev = 0;
  const size_t len = input.size();
  for (size_t i = 0; i < len; ++i) {
    delta_buffer[i] = input[i] - prev;
    prev = input[i];
  }

  double raw_entropy = get_entropy(input);
  double delta_entropy = get_entropy(delta_buffer);

  // Delta is strong for numeric/sensor data
  if (delta_entropy < raw_entropy - 0.5) {
    return {CompressorType::DELTA_FSE, CompressorType::FUZZY_LZ_3B,
            CompressorType::LZ_3B, CompressorType::LZ_2B, CompressorType::FSE};
  }

  // Refined Logic:
  // Source code/Text usually has entropy 4.5 - 6.5.
  // Binaries/Encrypted data have entropy > 7.5.
  // LZ is usually superior for anything structured, even if entropy is somewhat
  // high. We only deprioritize LZ if entropy is extremely high (indicating
  // randomness).

  if (raw_entropy < 7.5) {
    // Structured Data (Text, Code, Binaries, JSON, XML)
    // LZ_3B is the robust default. FuzzyLZ finds near-matches.
    return {CompressorType::LZ_3B, CompressorType::FUZZY_LZ_3B,
            CompressorType::LZ_2B, CompressorType::FSE,
            CompressorType::DELTA_FSE};
  } else {
    // High Randomness (Compressed data, Encrypted, High Entropy Noise)
    // FSE is the best bet to squeeze remaining bits.
    return {CompressorType::FSE, CompressorType::DELTA_FSE,
            CompressorType::FUZZY_LZ_3B, CompressorType::LZ_3B,
            CompressorType::LZ_2B};
  }
}
