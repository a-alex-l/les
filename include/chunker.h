#pragma once

#include <string>

class Chunker {
public:
    /**
     * @brief Default constructor.
     */
    Chunker() = default;

    /**
     * @brief Compresses a file sequentially (single-threaded).
     * 
     * The chunk size is dynamically calculated based on the compression level:
     * - Level 1: 128 KB
     * - Level 2: 256 KB
     * ...
     * - Level 9: 32 MB
     * 
     * This allows for faster processing on lower levels and better compression
     * context on higher levels.
     * 
     * @param input_file The path to the file to compress.
     * @param output_file The path where the compressed file will be saved.
     * @param level The compression level from 1 (low/fast) to 9 (high/slow).
     */
    void compress_file(const std::string& input_file, const std::string& output_file, int level);

    /**
     * @brief Decompresses a file sequentially.
     * 
     * Reads the chunk headers from the file to determine how to decompress each block.
     * 
     * @param input_file The path to the .les file to decompress.
     * @param output_file The path where the decompressed file will be saved.
     */
    void decompress_file(const std::string& input_file, const std::string& output_file);
};