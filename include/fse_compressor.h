#pragma once
#include <cstdint>
#include <span>

class FSECompressor {
public:
  /**
   * @brief Returns the maximum possible buffer size required to compress the
   * input.
   * @param input_size The size of the data to be compressed.
   * @return The worst-case size of the compressed data.
   */
  size_t get_max_compressed_size(size_t input_size) const;

  /**
   * @brief Returns the maximum possible buffer size required to compress the
   * input.
   * @param input_size The size of the data to be compressed.
   * @return The worst-case size of memory needed to compress data.
   */
  size_t get_compression_scratch_size(size_t input_size) const;

  /**
   * @brief Returns the maximum possible buffer size required to compress the
   * input.
   * @param input_size The size of the data to be compressed.
   * @return The worst-case size of the memory needed to decompress data.
   */
  size_t get_decompression_scratch_size(size_t output_size) const;

  /**
   * @brief Compresses data from a source span to a destination span.
   * Automatically splits input into 128KB blocks for optimal entropy context.
   *
   * @param input A span viewing the uncompressed data.
   * @param output A span viewing the buffer where compressed data will be
   * written.
   * @param scratch A span viewing the buffer where tmp data would be stored.
   * @param level The compression level (ignored, FSE is parameter-free).
   * @return The number of bytes written to the output span.
   * @throws std::runtime_error if the output buffer is too small.
   */
  size_t compress(std::span<const uint8_t> input, std::span<uint8_t> output,
                  std::span<uint8_t> scratch, int level);

  /**
   * @brief Decompresses data from a source span to a destination span.
   * Handles multiple internal blocks automatically.
   *
   * @param input A span viewing the compressed data.
   * @param output A span viewing the buffer for the decompressed data.
   * @param scratch A span viewing the where tmp data would be stored.
   * @return The number of bytes written to the output span.
   * @throws std::runtime_error on corruption or if the output buffer is too
   * small.
   */
  size_t decompress(std::span<const uint8_t> input, std::span<uint8_t> output,
                    std::span<uint8_t> scratch);
};
