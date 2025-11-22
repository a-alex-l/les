#include "compression_classifier.h"

std::vector<CompressorType> CompressionClassifier::get_best_candidates(const std::vector<uint8_t>& data) {
    // TODO: Implement real analysis (e.g., check for repetition for LZ,
    // analyze symbol frequency for Huffman/FSE).

    // For now, return all available compressors to be tested.
    return {
        CompressorType::LZ,
        CompressorType::FSE,
        CompressorType::HUFFMAN
    };
}
