#include "fse_compressor.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
    // Tuning Constants
    constexpr size_t FSE_BLOCK_SIZE = 256 * 1024; // 32KB Internal Blocks
    
    // rANS Constants
    constexpr int SCALE_BITS = 12;
    constexpr uint32_t PROB_TOTAL = 1 << SCALE_BITS; // 4096
    constexpr uint32_t PROB_MASK = PROB_TOTAL - 1;
    constexpr uint32_t RANS_L = 1 << 15; // Lower bound for state normalization

    // Header Flags
    constexpr uint8_t HEADER_MODE_DENSE = 0;
    constexpr uint8_t HEADER_MODE_SPARSE = 1;

    struct SymbolStats {
        uint32_t freq;
        uint32_t cum_freq;
    };

    // Simple memory arena helper
    template <typename T>
    std::span<T> alloc_span(std::span<uint8_t> &memory, size_t count) {
        size_t bytes = count * sizeof(T);
        if (memory.size() < bytes)
            throw std::bad_alloc();
        T *ptr = reinterpret_cast<T *>(memory.data());
        memory = memory.subspan(bytes);
        return std::span<T>(ptr, count);
    }
} // namespace

size_t FSECompressor::get_max_compressed_size(size_t input_size) const {
    size_t num_blocks = (input_size + FSE_BLOCK_SIZE - 1) / FSE_BLOCK_SIZE;
    if (num_blocks == 0) num_blocks = 1;
    // Global Header (8) + Block Headers (~600 * N) + Input + Padding
    return 8 + input_size + (num_blocks * 600) + 64;
}

size_t FSECompressor::get_compression_scratch_size(size_t input_size) const {
    (void)input_size;
    // Reuse scratch for each block.
    return (256 * sizeof(uint64_t)) + (256 * sizeof(SymbolStats)) + FSE_BLOCK_SIZE + 256;
}

size_t FSECompressor::get_decompression_scratch_size(size_t output_size) const {
    (void)output_size;
    return (256 * sizeof(SymbolStats)) + PROB_TOTAL;
}

size_t FSECompressor::compress(std::span<const uint8_t> input,
                               std::span<uint8_t> output,
                               std::span<uint8_t> scratch, 
                               int level) {
    if (input.empty()) return 0;
    (void)level;

    size_t input_pos = 0;
    size_t output_pos = 0;

    // --- 1. Write Global Total Size ---
    if (output.size() < 8) throw std::runtime_error("Output buffer too small");
    uint64_t total_size = input.size();
    std::memcpy(&output[output_pos], &total_size, 8);
    output_pos += 8;

    // --- 2. Process Blocks ---
    while (input_pos < input.size()) {
        size_t chunk_len = std::min(FSE_BLOCK_SIZE, input.size() - input_pos);
        std::span<const uint8_t> block_in = input.subspan(input_pos, chunk_len);
        
        std::span<uint8_t> block_scratch = scratch;

        // Allocation
        auto raw_counts = alloc_span<uint64_t>(block_scratch, 256);
        std::fill(raw_counts.begin(), raw_counts.end(), 0);

        auto table = alloc_span<SymbolStats>(block_scratch, 256);
        auto stream_buffer = alloc_span<uint8_t>(block_scratch, chunk_len + 64);

        // Stats
        for (uint8_t b : block_in) raw_counts[b]++;

        // Normalization
        uint32_t current_sum = 0;
        int max_symbol_idx = -1;
        uint32_t max_freq = 0;

        for (int i = 0; i < 256; ++i) {
            if (raw_counts[i] == 0) {
                table[i].freq = 0;
            } else {
                uint64_t scaled = (raw_counts[i] * PROB_TOTAL) / chunk_len;
                if (scaled == 0) scaled = 1; 
                if (scaled > PROB_TOTAL) scaled = PROB_TOTAL;
                
                table[i].freq = static_cast<uint32_t>(scaled);
                current_sum += table[i].freq;
                
                if (table[i].freq > max_freq) {
                    max_freq = table[i].freq;
                    max_symbol_idx = i;
                }
            }
        }

        if (current_sum < PROB_TOTAL) {
            if (max_symbol_idx != -1)
                table[max_symbol_idx].freq += (PROB_TOTAL - current_sum);
            else
                table[0].freq += (PROB_TOTAL - current_sum);
        } else if (current_sum > PROB_TOTAL) {
            uint32_t excess = current_sum - PROB_TOTAL;
            if (max_symbol_idx != -1 && table[max_symbol_idx].freq > excess + 1) {
                table[max_symbol_idx].freq -= excess;
            } else {
                while (excess > 0) {
                    for (int i = 0; i < 256 && excess > 0; ++i) {
                        if (table[i].freq > 1) {
                            table[i].freq--;
                            excess--;
                        }
                    }
                }
            }
        }

        uint32_t accum = 0;
        int non_zero_symbols = 0;
        for (int i = 0; i < 256; ++i) {
            table[i].cum_freq = accum;
            accum += table[i].freq;
            if (table[i].freq > 0) non_zero_symbols++;
        }

        // Encoding
        size_t stream_pos = 0;
        uint32_t state = RANS_L;

        for (size_t i = chunk_len; i > 0; --i) {
            uint8_t sym = block_in[i - 1];
            uint32_t freq = table[sym].freq;
            uint32_t start = table[sym].cum_freq;

            uint32_t max_val = ((RANS_L >> SCALE_BITS) << 8) * freq;
            while (state >= max_val) {
                if (stream_pos >= stream_buffer.size()) 
                    throw std::runtime_error("FSE internal buffer overflow");
                stream_buffer[stream_pos++] = (state & 0xFF);
                state >>= 8;
            }
            state = ((state / freq) << SCALE_BITS) + start + (state % freq);
        }

        // Write Block Header
        size_t max_header = 600; 
        if (output_pos + max_header + stream_pos > output.size())
            throw std::runtime_error("Output buffer too small");

        uint64_t bs = chunk_len;
        std::memcpy(&output[output_pos], &bs, 8);
        output_pos += 8;

        size_t sparse_cost = 1 + 1 + (non_zero_symbols * 3);
        size_t dense_cost = 1 + 512;

        if (sparse_cost < dense_cost) {
            output[output_pos++] = HEADER_MODE_SPARSE;
            output[output_pos++] = static_cast<uint8_t>(non_zero_symbols);
            for (int i = 0; i < 256; ++i) {
                if (table[i].freq > 0) {
                    output[output_pos++] = static_cast<uint8_t>(i);
                    output[output_pos++] = table[i].freq & 0xFF;
                    output[output_pos++] = (table[i].freq >> 8) & 0xFF;
                }
            }
        } else {
            output[output_pos++] = HEADER_MODE_DENSE;
            for (int i = 0; i < 256; ++i) {
                uint16_t f = static_cast<uint16_t>(table[i].freq);
                output[output_pos++] = f & 0xFF;
                output[output_pos++] = (f >> 8) & 0xFF;
            }
        }

        std::memcpy(&output[output_pos], &state, 4);
        output_pos += 4;

        for (size_t i = 0; i < stream_pos; ++i) {
            output[output_pos++] = stream_buffer[stream_pos - 1 - i];
        }

        input_pos += chunk_len;
    }

    return output_pos;
}

size_t FSECompressor::decompress(std::span<const uint8_t> input,
                                 std::span<uint8_t> output,
                                 std::span<uint8_t> scratch) {
    if (input.empty()) return 0;
    
    size_t in_idx = 0;
    size_t out_idx = 0;

    // --- 1. Read Global Total Size ---
    if (in_idx + 8 > input.size()) throw std::runtime_error("Input too small for total size");
    uint64_t total_size;
    std::memcpy(&total_size, &input[in_idx], 8);
    in_idx += 8;

    if (total_size > output.size())
        throw std::runtime_error("Output buffer too small");

    // --- 2. Decode Blocks ---
    while (out_idx < total_size) {
        std::span<uint8_t> block_scratch = scratch;

        auto table = alloc_span<SymbolStats>(block_scratch, 256);
        auto slot_to_sym = alloc_span<uint8_t>(block_scratch, PROB_TOTAL);
        for(int i=0; i<256; ++i) table[i].freq = 0;

        // Block Header
        if (in_idx + 8 > input.size()) throw std::runtime_error("Input too small for block header");
        uint64_t block_size;
        std::memcpy(&block_size, &input[in_idx], 8);
        in_idx += 8;

        if (out_idx + block_size > total_size) 
            throw std::runtime_error("Block size exceeds expected total");

        // Mode
        if (in_idx >= input.size()) throw std::runtime_error("Input truncated");
        uint8_t mode = input[in_idx++];
        uint32_t accum = 0;

        if (mode == HEADER_MODE_SPARSE) {
            if (in_idx >= input.size()) throw std::runtime_error("Input truncated");
            uint8_t count = input[in_idx++];
            for (int k = 0; k < count; ++k) {
                if (in_idx + 3 > input.size()) throw std::runtime_error("Input truncated");
                uint8_t sym = input[in_idx++];
                uint32_t f = input[in_idx++];
                f |= (uint32_t(input[in_idx++]) << 8);
                table[sym].freq = f;
                table[sym].cum_freq = accum;
                std::memset(&slot_to_sym[accum], sym, f);
                accum += f;
            }
        } else {
            if (in_idx + 512 > input.size()) throw std::runtime_error("Input truncated");
            for (int i = 0; i < 256; ++i) {
                uint32_t f = input[in_idx++];
                f |= (uint32_t(input[in_idx++]) << 8);
                table[i].freq = f;
                table[i].cum_freq = accum;
                if (f > 0) std::memset(&slot_to_sym[accum], i, f);
                accum += f;
            }
        }

        if (accum != PROB_TOTAL) throw std::runtime_error("Table sum mismatch");

        // State
        if (in_idx + 4 > input.size()) throw std::runtime_error("Input truncated");
        uint32_t state;
        std::memcpy(&state, &input[in_idx], 4);
        in_idx += 4;

        // Decode Loop
        for (size_t i = 0; i < block_size; ++i) {
            uint32_t slot = state & PROB_MASK;
            uint8_t sym = slot_to_sym[slot];
            output[out_idx + i] = sym;

            uint32_t freq = table[sym].freq;
            uint32_t cum_freq = table[sym].cum_freq;
            
            state = freq * (state >> SCALE_BITS) + (slot - cum_freq);

            while (state < RANS_L) {
                if (in_idx >= input.size()) 
                    throw std::runtime_error("Unexpected end of stream");
                state = (state << 8) | input[in_idx++];
            }
        }
        
        out_idx += block_size;
    }

    return out_idx;
}