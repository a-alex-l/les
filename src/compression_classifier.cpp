#include "compression_classifier.h"
#include "common/entropy.h"
#include <vector>
#include <cmath>
#include <algorithm>

std::array<CompressorType, 4> CompressionClassifier::get_best_candidates(
    std::span<const uint8_t> input, 
    std::span<uint8_t> delta_buffer) 
{
    if (input.empty())
        return {CompressorType::NONE, CompressorType::NONE, CompressorType::NONE, CompressorType::NONE};

    uint8_t prev = 0;
    const size_t len = input.size();
    for (size_t i = 0; i < len; ++i) {
        delta_buffer[i] = input[i] - prev;
        prev = input[i];
    }

    double raw_entropy = get_entropy(input);
    double delta_entropy = get_entropy(delta_buffer);

    // Delta is usually a strong indicator of structure
    if (delta_entropy < raw_entropy - 0.5) { 
        return { CompressorType::DELTA_FSE, CompressorType::LZ_3B, CompressorType::LZ_2B, CompressorType::FSE };
    }
    
    if (raw_entropy < 6.0) {
        // Low entropy/Structured
        // LZ_3B is usually safer for mixed data, but we can try LZ_2B if we want speed/small data
        return { CompressorType::LZ_3B, CompressorType::LZ_2B, CompressorType::FSE, CompressorType::HUFFMAN };
    } else {
        // High entropy
        return { CompressorType::FSE, CompressorType::LZ_3B, CompressorType::LZ_2B, CompressorType::HUFFMAN };
    }
}