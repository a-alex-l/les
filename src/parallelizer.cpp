#include "parallelizer.h"
#include "compression_classifier.h"
#include "compressor_types.h"
#include "huffman_compressor.h"
#include "fse_compressor.h"
#include "lz_compressor.h"

#include <fstream>
#include <vector>
#include <thread>
#include <future>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <span> // Required for the new interfaces

// A helper struct to hold results from the compression threads
struct CompressionResult {
    CompressorType type = CompressorType::NONE;
    std::vector<uint8_t> data;
};

/**
 * @brief (Internal) Maps compression level (1-9) to a power-of-2 chunk size.
 */
static size_t get_chunk_size_for_level(int level) {
    return static_cast<size_t>(std::pow(2, 15 + level));
}

void Parallelizer::compress_file(const std::string& input_file, const std::string& output_file, int level) {
    const size_t chunk_size = get_chunk_size_for_level(level);

    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    // Read file into chunks
    std::vector<std::vector<uint8_t>> chunks;
    while (in) {
        std::vector<uint8_t> chunk(chunk_size);
        in.read(reinterpret_cast<char*>(chunk.data()), chunk_size);
        chunk.resize(in.gcount());
        if (!chunk.empty()) {
            chunks.push_back(std::move(chunk));
        }
    }
    in.close();

    CompressionClassifier classifier;
    std::vector<std::future<CompressionResult>> futures;

    for (const auto& chunk : chunks) {
        futures.push_back(std::async(std::launch::async, [&, chunk, level]() {
            auto candidates = classifier.get_best_candidates(chunk);
            CompressionResult best_result;

            // Pre-allocate a buffer for the best result to avoid re-allocations
            std::vector<uint8_t> best_compressed_data;

            for (auto type : candidates) {
                std::vector<uint8_t> current_compressed_data;
                size_t current_compressed_size = 0;

                // --- NEW SPAN-BASED LOGIC ---
                switch (type) {
                    case CompressorType::HUFFMAN: {
                        HuffmanCompressor c;
                        current_compressed_data.resize(c.get_max_compressed_size(chunk.size()));
                        current_compressed_size = c.compress(chunk, current_compressed_data, level);
                        break;
                    }
                    case CompressorType::FSE: {
                        FSECompressor c;
                        current_compressed_data.resize(c.get_max_compressed_size(chunk.size()));
                        current_compressed_size = c.compress(chunk, current_compressed_data, level);
                        break;
                    }
                    case CompressorType::LZ: {
                        LZCompressor c;
                        current_compressed_data.resize(c.get_max_compressed_size(chunk.size()));
                        current_compressed_size = c.compress(chunk, current_compressed_data, level);
                        break;
                    }
                    default: break;
                }
                current_compressed_data.resize(current_compressed_size);
                // --- END OF NEW LOGIC ---

                if (best_compressed_data.empty() || current_compressed_data.size() < best_compressed_data.size()) {
                    best_compressed_data = std::move(current_compressed_data);
                    best_result.type = type;
                }
            }

            // If compression was ineffective, store the chunk uncompressed
            if (!best_compressed_data.empty() && best_compressed_data.size() >= chunk.size()) {
                best_result.type = CompressorType::NONE;
                best_result.data = chunk;
            } else {
                best_result.data = std::move(best_compressed_data);
            }

            return best_result;
        }));
    }

    std::ofstream out(output_file, std::ios::binary);
    for (size_t i = 0; i < futures.size(); ++i) {
        auto result = futures[i].get();
        uint64_t compressed_size = result.data.size();
        uint64_t original_size = chunks[i].size(); // Get the original size

        // Write the new file format header for the chunk
        out.write(reinterpret_cast<const char*>(&result.type), sizeof(result.type));
        out.write(reinterpret_cast<const char*>(&compressed_size), sizeof(compressed_size));
        out.write(reinterpret_cast<const char*>(&original_size), sizeof(original_size));
        out.write(reinterpret_cast<const char*>(result.data.data()), result.data.size());
    }
}

void Parallelizer::decompress_file(const std::string& input_file, const std::string& output_file) {
    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    struct CompressedChunkInfo {
        CompressorType type;
        uint64_t original_size;
        std::vector<uint8_t> data;
    };

    std::vector<CompressedChunkInfo> compressed_chunks;
    while (in) {
        CompressorType type;
        uint64_t compressed_size;
        uint64_t original_size;

        in.read(reinterpret_cast<char*>(&type), sizeof(type));
        if (in.gcount() == 0) break;

        in.read(reinterpret_cast<char*>(&compressed_size), sizeof(compressed_size));
        if (in.gcount() != sizeof(compressed_size)) throw std::runtime_error("Corrupt file: cannot read compressed size.");

        in.read(reinterpret_cast<char*>(&original_size), sizeof(original_size));
        if (in.gcount() != sizeof(original_size)) throw std::runtime_error("Corrupt file: cannot read original size.");

        std::vector<uint8_t> data(compressed_size);
        in.read(reinterpret_cast<char*>(data.data()), compressed_size);
        if (in.gcount() != compressed_size) throw std::runtime_error("Corrupt file: chunk data is incomplete.");

        compressed_chunks.push_back({type, original_size, std::move(data)});
    }

    std::vector<std::future<std::vector<uint8_t>>> futures;
    for (const auto& chunk_info : compressed_chunks) {
        futures.push_back(std::async(std::launch::async, [&]() {
            // Pre-allocate the vector for the decompressed data
            std::vector<uint8_t> decompressed_data(chunk_info.original_size);
            
            // --- NEW SPAN-BASED DECOMPRESSION ---
            switch (chunk_info.type) {
                case CompressorType::NONE:
                    return chunk_info.data; // It was uncompressed, just return it
                case CompressorType::HUFFMAN: {
                    HuffmanCompressor c;
                    c.decompress(chunk_info.data, decompressed_data);
                    break;
                }
                case CompressorType::FSE: {
                    FSECompressor c;
                    c.decompress(chunk_info.data, decompressed_data);
                    break;
                }
                case CompressorType::LZ: {
                    LZCompressor c;
                    c.decompress(chunk_info.data, decompressed_data);
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported compressor type found in file.");
            }
            return decompressed_data;
        }));
    }

    std::ofstream out(output_file, std::ios::binary);
    for (auto& future : futures) {
        auto decompressed_chunk = future.get();
        out.write(reinterpret_cast<const char*>(decompressed_chunk.data()), decompressed_chunk.size());
    }
}