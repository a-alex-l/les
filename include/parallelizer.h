#pragma once

#include <string>

class Parallelizer {
public:
    /**
     * @brief Default constructor. The Parallelizer is ready to be used immediately.
     */
    Parallelizer() = default;

    /**
     * @brief Compresses a file in parallel.
     * 
     * The chunk size for parallelization is determined internally based on the
     * compression level. Higher levels use larger chunks for potentially
     * better compression ratios at the cost of more memory.
     * 
     * @param input_file The path to the file to compress.
     * @param output_file The path where the compressed file will be saved.
     * @param level The compression level from 1 (low and fast) to 9 (high and slow).
     */
    void compress_file(const std::string& input_file, const std::string& output_file, int level);

    /**
     * @brief Decompresses a file in parallel.
     * 
     * Chunk information is read directly from the compressed file's metadata.
     * 
     * @param input_file The path to the .les file to decompress.
     * @param output_file The path where the decompressed file will be saved.
     */
    void decompress_file(const std::string& input_file, const std::string& output_file);
};