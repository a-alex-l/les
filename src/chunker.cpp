#include "chunker.h"
#include "compression_classifier.h"
#include "compressor_types.h"
#include "huffman_compressor.h"
#include "fse_compressor.h"
#include "lz_compressor.h"
#include "fuzzy_lz_compressor.h"

#include <fstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <span>

namespace {
    size_t get_chunk_size_for_level(int level) {
        if (level > 9) level = 9;
        int shift = 15 + level;
        return 1ULL << shift;
    }
}

void Chunker::compress_file(const std::string& input_file, const std::string& output_file, int level) {
    const size_t chunk_size = get_chunk_size_for_level(level);

    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    std::ofstream out(output_file, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_file);

    CompressionClassifier classifier;

    std::vector<uint8_t> input_chunk(chunk_size);
    std::vector<uint8_t> delta_buffer(chunk_size);
    std::vector<uint8_t> compression_buffer;
    std::vector<uint8_t> best_buffer;

    while (in) {
        in.read(reinterpret_cast<char*>(input_chunk.data()), chunk_size);
        size_t bytes_read = in.gcount();
        if (bytes_read == 0) break;

        std::span<const uint8_t> raw_span(input_chunk.data(), bytes_read);
        std::span<uint8_t> delta_span(delta_buffer.data(), bytes_read);

        auto candidates = classifier.get_best_candidates(raw_span, delta_span);

        CompressorType best_type = CompressorType::NONE;
        best_buffer.resize(bytes_read);
        std::copy(raw_span.begin(), raw_span.end(), best_buffer.begin());
        size_t best_size = bytes_read;

        const size_t types_to_check = 1 + (level >= 4) + (level >= 7) + (level == 9) * (candidates.size() - 3);
        for (size_t type_id = 0; type_id < types_to_check; ++type_id) {
            auto type = candidates[type_id];
            size_t max_size = 0;
            size_t res_size = 0;

            switch (type) {
                case CompressorType::HUFFMAN: {
                    HuffmanCompressor c;
                    max_size = c.get_max_compressed_size(bytes_read);
                    break;
                }
                case CompressorType::LZ_2B: { 
                    LZCompressor<LZMode::V2B> c; 
                    max_size = c.get_max_compressed_size(bytes_read); 
                    break; 
                }
                case CompressorType::LZ_3B: { 
                    LZCompressor<LZMode::V3B> c; 
                    max_size = c.get_max_compressed_size(bytes_read); 
                    break; 
                }
                case CompressorType::FUZZY_LZ_3B: { 
                    FuzzyLZCompressor c; 
                    max_size = c.get_max_compressed_size(bytes_read); 
                    break; 
                }
                case CompressorType::FSE:
                case CompressorType::DELTA_FSE: {
                    FSECompressor c;
                    max_size = c.get_max_compressed_size(bytes_read);
                    break; }
                default: continue;
            }

            if (compression_buffer.size() < max_size) {
                compression_buffer.resize(max_size);
            }

            try {
                switch (type) {
                    case CompressorType::HUFFMAN: { 
                        HuffmanCompressor c; 
                        res_size = c.compress(raw_span, compression_buffer, level); 
                        break; 
                    }
                    case CompressorType::FSE: { 
                        FSECompressor c; 
                        res_size = c.compress(raw_span, compression_buffer, level); 
                        break; 
                    }
                    case CompressorType::LZ_2B: { 
                        LZCompressor<LZMode::V2B> c; 
                        res_size = c.compress(raw_span, compression_buffer, level); 
                        break; 
                    }
                    case CompressorType::LZ_3B: { 
                        LZCompressor<LZMode::V3B> c; 
                        res_size = c.compress(raw_span, compression_buffer, level); 
                        break; 
                    }
                    case CompressorType::DELTA_FSE: {
                        FSECompressor c;
                        res_size = c.compress(delta_span, compression_buffer, level);
                        break;
                    }
                    case CompressorType::FUZZY_LZ_3B: {
                        FuzzyLZCompressor c;
                        res_size = c.compress(delta_span, compression_buffer, level);
                        break;
                    }
                    default: break;
                }

                if (res_size > 0 && res_size < best_size) {
                    best_size = res_size;
                    best_type = type;
                    if (best_buffer.size() < best_size) best_buffer.resize(best_size);
                    std::copy(compression_buffer.begin(), compression_buffer.begin() + best_size, best_buffer.begin());
                }
            } catch (...) {
                continue;
            }
        }

        uint64_t w_c_size = best_size;
        uint64_t w_o_size = bytes_read;

        out.write(reinterpret_cast<const char*>(&best_type), sizeof(best_type));
        out.write(reinterpret_cast<const char*>(&w_c_size), sizeof(w_c_size));
        out.write(reinterpret_cast<const char*>(&w_o_size), sizeof(w_o_size));
        out.write(reinterpret_cast<const char*>(best_buffer.data()), best_size);
    }
}

void Chunker::decompress_file(const std::string& input_file, const std::string& output_file) {
    std::ifstream in(input_file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input file: " + input_file);

    std::ofstream out(output_file, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output file: " + output_file);

    std::vector<uint8_t> compressed_buffer;
    std::vector<uint8_t> decompressed_buffer;

    while (in.peek() != EOF) {
        CompressorType type;
        uint64_t c_size;
        uint64_t o_size;

        if (!in.read(reinterpret_cast<char*>(&type), sizeof(type))) break;
        in.read(reinterpret_cast<char*>(&c_size), sizeof(c_size));
        in.read(reinterpret_cast<char*>(&o_size), sizeof(o_size));

        if (compressed_buffer.size() < c_size) compressed_buffer.resize(c_size);
        in.read(reinterpret_cast<char*>(compressed_buffer.data()), c_size);

        if (in.gcount() != static_cast<std::streamsize>(c_size)) {
            throw std::runtime_error("File corrupted: Unexpected end of stream.");
        }

        if (decompressed_buffer.size() < o_size) decompressed_buffer.resize(o_size);

        std::span<const uint8_t> src_span(compressed_buffer.data(), c_size);
        std::span<uint8_t> dst_span(decompressed_buffer.data(), o_size);

        switch (type) {
            case CompressorType::NONE:
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
            case CompressorType::LZ_2B: {
                LZCompressor<LZMode::V2B> c;
                c.decompress(src_span, dst_span);
                break;
            }
            case CompressorType::LZ_3B: {
                LZCompressor<LZMode::V3B> c;
                c.decompress(src_span, dst_span);
                break;
            }
            case CompressorType::FUZZY_LZ_3B: {
                FuzzyLZCompressor c;
                c.decompress(src_span, dst_span);
                break;
            }
            case CompressorType::DELTA_FSE: {
                FSECompressor c;
                c.decompress(src_span, dst_span);

                uint8_t prev = 0;
                for (size_t i = 0; i < dst_span.size(); ++i) {
                    dst_span[i] = dst_span[i] + prev;
                    prev = dst_span[i];
                }
                break;
            }
            default:
                throw std::runtime_error("Unknown compressor type encountered.");
        }

        out.write(reinterpret_cast<const char*>(decompressed_buffer.data()), o_size);
    }
}
