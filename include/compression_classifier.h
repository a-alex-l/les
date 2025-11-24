#pragma once
#include "compressor_types.h"
#include <array>
#include <span>
#include <cstdint>

class CompressionClassifier {
public:
    /**
     * @brief Analyzes input, populates the delta_buffer, and determines best strategy.
     * 
     * @param input The raw input data.
     * @param delta_buffer A pre-allocated buffer (must be size >= input.size()).
     *                     The classifier WILL write the delta-transformed data here.
     * @return Ranked list of compression strategies.
     */
    std::array<CompressorType, 6> get_best_candidates(
        std::span<const uint8_t> input, 
        std::span<uint8_t> delta_buffer
    );
};