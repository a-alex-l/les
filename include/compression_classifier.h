#pragma once
#include "compressor_types.h"
#include <array>
#include <span>
#include <cstdint>

class CompressionClassifier {
public:
    /**
     * @brief Analyzes a chunk and returns a ranked list of the best
     *        compression candidates.
     * @param data The input data chunk.
     * @return A vector of CompressorType enums, ordered from best to worst.
     */
    std::array<CompressorType, 3> get_best_candidates(std::span<const uint8_t> input);
};
