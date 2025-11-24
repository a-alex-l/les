#include "chunker.h"
#include "compression_classifier.h"
#include "compressor_types.h"
#include "fse_compressor.h"
#include "fuzzy_lz_compressor.h"
#include "lz_compressor.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

namespace {
  size_t get_chunk_size_for_level(int level) {
    if (level > 9)
      level = 9;
    int shift = 15 + level;
    return 1ULL << shift;
  }

int get_trials_for_level(int level) {
  if (level <= 2) return 1; // Trust the classifier
  if (level <= 3) return 2; // Check top 2
  if (level <= 7) return 3; // Check top 3 (Covers FSE vs LZ ambiguity)
  return 5;                 // Check all
}
} // namespace

void Chunker::compress_file(const std::string &input_file,
                            const std::string &output_file, int level) {
  const size_t chunk_size = get_chunk_size_for_level(level);
  const int max_trials = get_trials_for_level(level);

  std::ifstream in(input_file, std::ios::binary);
  if (!in)
    throw std::runtime_error("Cannot open input file: " + input_file);
  std::ofstream out(output_file, std::ios::binary);
  if (!out)
    throw std::runtime_error("Cannot open output file: " + output_file);

  CompressionClassifier classifier;
  std::vector<uint8_t> input_chunk(chunk_size);
  std::vector<uint8_t> delta_buffer(chunk_size);
  std::vector<uint8_t> compression_buffer;
  std::vector<uint8_t> best_buffer;
  std::vector<uint8_t> scratch_buffer;

  while (in) {
    in.read(reinterpret_cast<char *>(input_chunk.data()), chunk_size);
    size_t bytes_read = in.gcount();
    if (bytes_read == 0)
      break;

    std::span<const uint8_t> raw_span(input_chunk.data(), bytes_read);
    std::span<uint8_t> delta_span(delta_buffer.data(), bytes_read);
    
    auto candidates = classifier.get_best_candidates(raw_span, delta_span);

    // Default to uncompressed
    CompressorType best_type = CompressorType::NONE;
    best_buffer.resize(bytes_read);
    std::copy(raw_span.begin(), raw_span.end(), best_buffer.begin());
    size_t best_size = bytes_read;

    int trials = std::min(max_trials, static_cast<int>(candidates.size()));

    for (int t = 0; t < trials; ++t) {
      CompressorType type = candidates[t];
      if (type == CompressorType::NONE) continue;

      size_t max_size = 0;
      size_t scratch_sz = 0;
      size_t res_size = 0;

      auto setup = [&](auto &c) {
        max_size = c.get_max_compressed_size(bytes_read);
        scratch_sz = c.get_compression_scratch_size(bytes_read);
      };

      switch (type) {
      case CompressorType::LZ_2B: {
        LZCompressor<LZMode::V2B> c; setup(c); break;
      }
      case CompressorType::LZ_3B: {
        LZCompressor<LZMode::V3B> c; setup(c); break;
      }
      case CompressorType::FUZZY_LZ_3B: {
        FuzzyLZCompressor c; setup(c); break;
      }
      case CompressorType::FSE:
      case CompressorType::DELTA_FSE: {
        FSECompressor c; setup(c); break;
      }
      default: continue;
      }

      if (compression_buffer.size() < max_size)
        compression_buffer.resize(max_size);
      
      // Ensure scratch is large enough
      if (scratch_buffer.size() < scratch_sz)
        scratch_buffer.resize(scratch_sz);

      try {
        std::span<uint8_t> scratch_span(scratch_buffer.data(), scratch_sz);
        switch (type) {
        case CompressorType::FSE: {
          FSECompressor c;
          res_size = c.compress(raw_span, compression_buffer, scratch_span, level);
          break;
        }
        case CompressorType::LZ_2B: {
          LZCompressor<LZMode::V2B> c;
          res_size = c.compress(raw_span, compression_buffer, scratch_span, level);
          break;
        }
        case CompressorType::LZ_3B: {
          LZCompressor<LZMode::V3B> c;
          res_size = c.compress(raw_span, compression_buffer, scratch_span, level);
          break;
        }
        case CompressorType::DELTA_FSE: {
          FSECompressor c;
          res_size = c.compress(delta_span, compression_buffer, scratch_span, level);
          break;
        }
        case CompressorType::FUZZY_LZ_3B: {
          FuzzyLZCompressor c;
          res_size = c.compress(delta_span, compression_buffer, scratch_span, level);
          break;
        }
        default: break;
        }

        if (res_size > 0 && res_size < best_size) {
          best_size = res_size;
          best_type = type;
          if (best_buffer.size() < best_size)
            best_buffer.resize(best_size);
          std::copy(compression_buffer.begin(),
                    compression_buffer.begin() + best_size,
                    best_buffer.begin());
        }
      } catch (...) {
        // Safe fallback
        continue;
      }
    }

    uint64_t w_c_size = best_size;
    uint64_t w_o_size = bytes_read;
    out.write(reinterpret_cast<const char *>(&best_type), sizeof(best_type));
    out.write(reinterpret_cast<const char *>(&w_c_size), sizeof(w_c_size));
    out.write(reinterpret_cast<const char *>(&w_o_size), sizeof(w_o_size));
    out.write(reinterpret_cast<const char *>(best_buffer.data()), best_size);
  }
}

void Chunker::decompress_file(const std::string &input_file,
                              const std::string &output_file) {
  std::ifstream in(input_file, std::ios::binary);
  if (!in)
    throw std::runtime_error("Cannot open input file: " + input_file);
  std::ofstream out(output_file, std::ios::binary);
  if (!out)
    throw std::runtime_error("Cannot open output file: " + output_file);

  std::vector<uint8_t> compressed_buffer;
  std::vector<uint8_t> decompressed_buffer;
  std::vector<uint8_t> scratch_buffer;

  while (in.peek() != EOF) {
    CompressorType type;
    uint64_t c_size;
    uint64_t o_size;
    if (!in.read(reinterpret_cast<char *>(&type), sizeof(type)))
      break;
    in.read(reinterpret_cast<char *>(&c_size), sizeof(c_size));
    in.read(reinterpret_cast<char *>(&o_size), sizeof(o_size));

    if (compressed_buffer.size() < c_size)
      compressed_buffer.resize(c_size);
    in.read(reinterpret_cast<char *>(compressed_buffer.data()), c_size);
    if (in.gcount() != static_cast<std::streamsize>(c_size))
      throw std::runtime_error("File corrupted");

    if (decompressed_buffer.size() < o_size)
      decompressed_buffer.resize(o_size);

    std::span<const uint8_t> src_span(compressed_buffer.data(), c_size);
    std::span<uint8_t> dst_span(decompressed_buffer.data(), o_size);

    size_t scratch_req = 0;
    switch (type) {
    case CompressorType::FSE:
    case CompressorType::DELTA_FSE: {
      FSECompressor c;
      scratch_req = c.get_decompression_scratch_size(o_size);
      break;
    }
    case CompressorType::LZ_2B: {
      LZCompressor<LZMode::V2B> c;
      scratch_req = c.get_decompression_scratch_size(o_size);
      break;
    }
    case CompressorType::LZ_3B: {
      LZCompressor<LZMode::V3B> c;
      scratch_req = c.get_decompression_scratch_size(o_size);
      break;
    }
    case CompressorType::FUZZY_LZ_3B: {
      FuzzyLZCompressor c;
      scratch_req = c.get_decompression_scratch_size(o_size);
      break;
    }
    default:
      break;
    }

    if (scratch_buffer.size() < scratch_req)
      scratch_buffer.resize(scratch_req);
    std::span<uint8_t> scratch(scratch_buffer.data(), scratch_req);

    switch (type) {
    case CompressorType::NONE:
      std::copy(src_span.begin(), src_span.end(), dst_span.begin());
      break;
    case CompressorType::FSE: {
      FSECompressor c;
      c.decompress(src_span, dst_span, scratch);
      break;
    }
    case CompressorType::LZ_2B: {
      LZCompressor<LZMode::V2B> c;
      c.decompress(src_span, dst_span, scratch);
      break;
    }
    case CompressorType::LZ_3B: {
      LZCompressor<LZMode::V3B> c;
      c.decompress(src_span, dst_span, scratch);
      break;
    }
    case CompressorType::FUZZY_LZ_3B: {
      FuzzyLZCompressor c;
      c.decompress(src_span, dst_span, scratch);
      break;
    }
    case CompressorType::DELTA_FSE: {
      FSECompressor c;
      c.decompress(src_span, dst_span, scratch);
      uint8_t prev = 0;
      for (size_t i = 0; i < dst_span.size(); ++i) {
        dst_span[i] += prev;
        prev = dst_span[i];
      }
      break;
    }
    default:
      throw std::runtime_error("Unknown compressor type");
    }
    out.write(reinterpret_cast<const char *>(decompressed_buffer.data()),
              o_size);
  }
}
