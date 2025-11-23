#pragma once
#include <span>
#include <cstdint>
#include <cstddef>

enum class LZMode {
    V2B, // Version 2-Byte (4KB Window)
    V3B  // Version 3-Byte (64KB Window)
};

template <LZMode Mode>
class LZCompressor {
public:

    /**
     * @brief Returns the maximum possible buffer size required to compress the input.
     * @param input_size The size of the data to be compressed.
     * @return The worst-case size of the compressed data.
     */
    size_t get_max_compressed_size(size_t input_size) const;

    /**
     * @brief Compresses data from a source span to a destination span.
     * @param input A span viewing the uncompressed data.
     * @param output A span viewing the buffer where compressed data will be written.
     * @param level The compression level (1-9).
     * @return The number of bytes written to the output span.
     * @throws std::runtime_error if the output buffer is too small.
     */
    size_t compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level);

    /**
     * @brief Decompresses data from a source span to a destination span.
     * @param input A span viewing the compressed data.
     * @param output A span viewing the buffer for the decompressed data.
     * @return The number of bytes written to the output span.
     * @throws std::runtime_error on corruption or if the output buffer is too small.
     */
    size_t decompress(std::span<const uint8_t> input, std::span<uint8_t> output);
};