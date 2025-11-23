#include "parallelizer.h"
#include "compression_classifier.h"
#include "compressor_types.h"
#include "huffman_compressor.h"
#include "fse_compressor.h"
#include "lz_compressor.h"

#include <fstream>
#include <vector>
#include <iostream>
#include <cmath>
#include <algorithm>

namespace {
    // 128KB = 131072 bytes
    // Level 1: 128KB << 0 = 128KB
    // Level 2: 128KB << 1 = 256KB
    // ...
    // Level 9: 128KB << 8 = 32MB
    size_t get_chunk_size_for_level(int level) {
        if (level > 9) level = 9;
        int shift = 15 + level;
        return 1ULL << shift;
    }
}

void Parallelizer::compress_file(const std::string& input_file, const std::string& output_file, int level) {
    const size_t chunk_size = get_chunk_size_for_level(level);

    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    std::ofstream out(output_file, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_file);

    CompressionClassifier classifier;

    // Reusable buffers to minimize memory allocations
    std::vector<uint8_t> input_chunk(chunk_size);
    std::vector<uint8_t> compression_buffer; 
    std::vector<uint8_t> best_buffer;

    while (in) {
        // 1. Read Chunk
        in.read(reinterpret_cast<char*>(input_chunk.data()), chunk_size);
        size_t bytes_read = in.gcount();
        if (bytes_read == 0) break;

        // Create a view of the actual data read
        std::span<const uint8_t> current_span(input_chunk.data(), bytes_read);

        // 2. Classify
        // We make a temporary vector for the classifier because it currently takes vector const&
        // (Optimization: modify classifier to take span in the future to avoid this copy)
        std::vector<uint8_t> classifier_input(input_chunk.begin(), input_chunk.begin() + bytes_read);
        auto candidates = classifier.get_best_candidates(classifier_input);

        // 3. Find Best Compressor
        CompressorType best_type = CompressorType::NONE;
        
        // Default to "None" (store uncompressed) initially
        best_buffer.resize(bytes_read);
        std::copy(current_span.begin(), current_span.end(), best_buffer.begin());
        size_t best_size = bytes_read;
        bool compressed_successfully = false;

        for (auto type : candidates) {
            size_t max_size = 0;
            size_t res_size = 0;

            // Ensure buffer is large enough
            switch (type) {
                case CompressorType::HUFFMAN: { HuffmanCompressor c; max_size = c.get_max_compressed_size(bytes_read); break; }
                case CompressorType::FSE:     { FSECompressor c;     max_size = c.get_max_compressed_size(bytes_read); break; }
                case CompressorType::LZ:      { LZCompressor c;      max_size = c.get_max_compressed_size(bytes_read); break; }
                default: continue;
            }

            if (compression_buffer.size() < max_size) {
                compression_buffer.resize(max_size);
            }

            try {
                switch (type) {
                    case CompressorType::HUFFMAN: { HuffmanCompressor c; res_size = c.compress(current_span, compression_buffer, level); break; }
                    case CompressorType::FSE:     { FSECompressor c;     res_size = c.compress(current_span, compression_buffer, level); break; }
                    case CompressorType::LZ:      { LZCompressor c;      res_size = c.compress(current_span, compression_buffer, level); break; }
                    default: break;
                }

                // If this result is valid AND smaller than the original AND smaller than previous best
                if (res_size > 0 && res_size < best_size) {
                    best_size = res_size;
                    best_type = type;
                    
                    // Copy result to best_buffer
                    if (best_buffer.size() < best_size) best_buffer.resize(best_size);
                    std::copy(compression_buffer.begin(), compression_buffer.begin() + best_size, best_buffer.begin());
                    compressed_successfully = true;
                }
            } catch (...) {
                // If a compressor fails (e.g. buffer issues), just skip it
                continue;
            }
        }

        // 4. Write to Output
        // Header: [Type (4)] [Compressed Size (8)] [Original Size (8)] [Data...]
        uint64_t w_c_size = best_size;
        uint64_t w_o_size = bytes_read;

        out.write(reinterpret_cast<const char*>(&best_type), sizeof(best_type));
        out.write(reinterpret_cast<const char*>(&w_c_size), sizeof(w_c_size));
        out.write(reinterpret_cast<const char*>(&w_o_size), sizeof(w_o_size));
        out.write(reinterpret_cast<const char*>(best_buffer.data()), best_size);
    }
}

void Parallelizer::decompress_file(const std::string& input_file, const std::string& output_file) {
    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    std::ofstream out(output_file, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_file);

    // Reusable buffers
    std::vector<uint8_t> compressed_buffer;
    std::vector<uint8_t> decompressed_buffer;

    while (in.peek() != EOF) {
        CompressorType type;
        uint64_t c_size;
        uint64_t o_size;

        // 1. Read Header
        if (!in.read(reinterpret_cast<char*>(&type), sizeof(type))) break;
        in.read(reinterpret_cast<char*>(&c_size), sizeof(c_size));
        in.read(reinterpret_cast<char*>(&o_size), sizeof(o_size));

        // 2. Read Compressed Data
        if (compressed_buffer.size() < c_size) compressed_buffer.resize(c_size);
        in.read(reinterpret_cast<char*>(compressed_buffer.data()), c_size);

        if (in.gcount() != static_cast<std::streamsize>(c_size)) {
            throw std::runtime_error("File corrupted: Unexpected end of stream.");
        }

        // 3. Decompress
        if (decompressed_buffer.size() < o_size) decompressed_buffer.resize(o_size);

        std::span<const uint8_t> src_span(compressed_buffer.data(), c_size);
        std::span<uint8_t> dst_span(decompressed_buffer.data(), o_size);

        switch (type) {
            case CompressorType::NONE:
                // Direct copy
                std::copy(src_span.begin(), src_span.end(), dst_span.begin());
                break;
            case CompressorType::HUFFMAN: {
                HuffmanCompressor c;
                c.decompress(src_span, dst_span);
                break;
            }
            case CompressorType::FSE: {
                FSECompressor c;
                c.decompress(src_span, dst_span);
                break;
            }
            case CompressorType::LZ: {
                LZCompressor c;
                c.decompress(src_span, dst_span);
                break;
            }
            default:
                throw std::runtime_error("Unknown compressor type encountered.");
        }

        // 4. Write Decompressed Data
        out.write(reinterpret_cast<const char*>(decompressed_buffer.data()), o_size);
    }
}