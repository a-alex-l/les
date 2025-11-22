#pragma once
#include "compressor_types.h"
#include <vector>
#include <cstdint>

class CompressionClassifier {
public:
    /**
     * @brief Analyzes a chunk and returns a ranked list of the best
     *        compression candidates.
     * @param data The input data chunk.
     * @return A vector of CompressorType enums, ordered from best to worst.
     */
    std::vector<CompressorType> get_best_candidates(const std::vector<uint8_t>& data);
};
