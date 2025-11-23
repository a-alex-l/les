#include "compression_classifier.h"
#include "common/entropy.h"
#include <vector>
#include <cmath>
#include <algorithm>

std::array<CompressorType, 3> CompressionClassifier::get_best_candidates(std::span<const uint8_t> input) {
    if (input.empty())
        return {CompressorType::NONE, CompressorType::NONE, CompressorType::NONE};

    double entropy = get_entropy(input);

    if (entropy < 6.5) {
        return { CompressorType::LZ, CompressorType::HUFFMAN, CompressorType::FSE };
    } else {
        return { CompressorType::FSE, CompressorType::HUFFMAN, CompressorType::LZ };
    }
}