#pragma once
#include <span>
#include <cstdint>
#include <vector>

class FuzzyLZCompressor {
public:
    /**
     * @brief Returns worst-case buffer size. 
     * Since we split streams and add metadata, we need a safe margin.
     * @param input_size The size of the data to be compressed.
     * @return The worst-case size of the compressed data.
     */
    size_t get_max_compressed_size(size_t input_size) const;

    /**
     * @brief Compresses using the "Fuzzy Match" algorithm.
     * 1. LZ Search with SIMD Even/Odd Hashing.
     * 2. Fuzzy Matching based on Hamming Distance (PopCount).
     * 3. Splitting results into Huffman (Structure) and FSE (Deltas).
     * @param input A span viewing the uncompressed data.
     * @param output A span viewing the buffer where compressed data will be written.
     * @param level The compression level (1-9).
     * @return The number of bytes written to the output span.
     * @throws std::runtime_error if the output buffer is too small.
     */
    size_t compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level);

    /**
     * @brief Decompresses and applies the XOR patches.
     * @param input A span viewing the compressed data.
     * @param output A span viewing the buffer for the decompressed data.
     * @return The number of bytes written to the output span.
     * @throws std::runtime_error on corruption or if the output buffer is too small.
     */
    size_t decompress(std::span<const uint8_t> input, std::span<uint8_t> output);
};
