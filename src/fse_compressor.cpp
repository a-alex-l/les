#include "fse_compressor.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
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
    // Input + Header (Size 8 + Mode 1 + Table 512 + State 4) + Margin
    return input_size + 600;
}

size_t FSECompressor::get_compression_scratch_size(size_t input_size) const {
    // raw_counts (256*8) + table (256*8) + stream_buffer (input_size)
    return (256 * sizeof(uint64_t)) + (256 * sizeof(SymbolStats)) + input_size;
}

size_t FSECompressor::get_decompression_scratch_size(size_t output_size) const {
    (void)output_size;
    // table (256*8) + slot_lookup (4096)
    return (256 * sizeof(SymbolStats)) + PROB_TOTAL;
}

size_t FSECompressor::compress(std::span<const uint8_t> input,
                               std::span<uint8_t> output,
                               std::span<uint8_t> scratch, 
                               int level) {
    if (input.empty()) return 0;
    (void)level;

    // --- 1. Allocation ---
    auto raw_counts = alloc_span<uint64_t>(scratch, 256);
    std::fill(raw_counts.begin(), raw_counts.end(), 0);

    auto table = alloc_span<SymbolStats>(scratch, 256);
    auto stream_buffer = alloc_span<uint8_t>(scratch, input.size());

    // --- 2. Frequency Counting ---
    for (uint8_t b : input) raw_counts[b]++;

    // --- 3. Normalization (Scale to 4096) ---
    uint32_t current_sum = 0;
    int max_symbol_idx = -1;
    uint32_t max_freq = 0;

    for (int i = 0; i < 256; ++i) {
        if (raw_counts[i] == 0) {
            table[i].freq = 0;
        } else {
            // Basic scaling
            uint64_t scaled = (raw_counts[i] * PROB_TOTAL) / input.size();
            if (scaled == 0) scaled = 1; // Must exist
            if (scaled > PROB_TOTAL) scaled = PROB_TOTAL;
            
            table[i].freq = static_cast<uint32_t>(scaled);
            current_sum += table[i].freq;
            
            if (table[i].freq > max_freq) {
                max_freq = table[i].freq;
                max_symbol_idx = i;
            }
        }
    }

    // Adjust to sum exactly to PROB_TOTAL (4096)
    if (current_sum < PROB_TOTAL) {
        if (max_symbol_idx != -1)
            table[max_symbol_idx].freq += (PROB_TOTAL - current_sum);
        else
            return 0; // Empty input or error
    } else if (current_sum > PROB_TOTAL) {
        uint32_t excess = current_sum - PROB_TOTAL;
        // Try to take from largest
        if (max_symbol_idx != -1 && table[max_symbol_idx].freq > excess + 1) {
            table[max_symbol_idx].freq -= excess;
        } else {
            // Distribute substraction
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

    // Build Cumulative Frequencies
    uint32_t accum = 0;
    int non_zero_symbols = 0;
    for (int i = 0; i < 256; ++i) {
        table[i].cum_freq = accum;
        accum += table[i].freq;
        if (table[i].freq > 0) non_zero_symbols++;
    }

    if (accum != PROB_TOTAL) throw std::runtime_error("Normalization failed");

    // --- 4. Encoding (Reverse) ---
    size_t stream_pos = 0;
    uint32_t state = RANS_L; // Initial state

    for (size_t i = input.size(); i > 0; --i) {
        uint8_t sym = input[i - 1];
        uint32_t freq = table[sym].freq;
        uint32_t start = table[sym].cum_freq;

        // rANS renormalization: output bytes to bring state within range
        uint32_t max_val = ((RANS_L >> SCALE_BITS) << 8) * freq;
        while (state >= max_val) {
            stream_buffer[stream_pos++] = (state & 0xFF);
            state >>= 8;
        }
        // Update state
        state = ((state / freq) << SCALE_BITS) + start + (state % freq);
    }

    // --- 5. Write Header & Output ---
    size_t out_idx = 0;
    if (output.size() < 16) throw std::runtime_error("Output buffer too small");

    // 5.1 Write Original Size (8 bytes)
    uint64_t os = input.size();
    std::memcpy(&output[out_idx], &os, 8);
    out_idx += 8;

    // 5.2 Write Frequency Table (Smart Selection)
    // Sparse Cost: 1 (Flag) + 1 (Count) + (Symbols * 3)
    // Dense Cost:  1 (Flag) + 512
    size_t sparse_cost = 1 + 1 + (non_zero_symbols * 3);
    size_t dense_cost = 1 + 512;

    if (sparse_cost < dense_cost) {
        // Write Sparse
        output[out_idx++] = HEADER_MODE_SPARSE;
        output[out_idx++] = static_cast<uint8_t>(non_zero_symbols);
        
        for (int i = 0; i < 256; ++i) {
            if (table[i].freq > 0) {
                output[out_idx++] = static_cast<uint8_t>(i);    // Symbol
                output[out_idx++] = table[i].freq & 0xFF;       // Low Byte
                output[out_idx++] = (table[i].freq >> 8) & 0xFF;// High Byte
            }
        }
    } else {
        // Write Dense
        output[out_idx++] = HEADER_MODE_DENSE;
        for (int i = 0; i < 256; ++i) {
            uint16_t f = static_cast<uint16_t>(table[i].freq);
            output[out_idx++] = f & 0xFF;
            output[out_idx++] = (f >> 8) & 0xFF;
        }
    }

    // 5.3 Write Final State (4 bytes)
    std::memcpy(&output[out_idx], &state, 4);
    out_idx += 4;

    // 5.4 Write Stream (Reverse Copy)
    if (output.size() < out_idx + stream_pos) 
        throw std::runtime_error("Output buffer too small for stream");

    for (size_t i = 0; i < stream_pos; ++i) {
        output[out_idx++] = stream_buffer[stream_pos - 1 - i];
    }

    return out_idx;
}

size_t FSECompressor::decompress(std::span<const uint8_t> input,
                                 std::span<uint8_t> output,
                                 std::span<uint8_t> scratch) {
    if (input.empty()) return 0;
    size_t in_idx = 0;

    // --- 1. Scratch Setup ---
    auto table = alloc_span<SymbolStats>(scratch, 256);
    // Inverse mapping: slot (0..4095) -> symbol
    auto slot_to_sym = alloc_span<uint8_t>(scratch, PROB_TOTAL);

    // Clear table initially
    for(int i=0; i<256; ++i) table[i].freq = 0;

    // --- 2. Read Header ---
    if (input.size() < 8) throw std::runtime_error("Input too small");
    
    uint64_t original_size;
    std::memcpy(&original_size, &input[in_idx], 8);
    in_idx += 8;

    if (output.size() < original_size)
        throw std::runtime_error("Output buffer too small");

    // Read Mode Flag
    if (in_idx >= input.size()) throw std::runtime_error("Input truncated");
    uint8_t mode = input[in_idx++];

    uint32_t accum = 0;

    if (mode == HEADER_MODE_SPARSE) {
        // --- Sparse Mode ---
        if (in_idx >= input.size()) throw std::runtime_error("Input truncated");
        uint8_t count = input[in_idx++]; // Number of symbols

        for (int k = 0; k < count; ++k) {
            if (in_idx + 3 > input.size()) throw std::runtime_error("Input truncated");
            
            uint8_t sym = input[in_idx++];
            uint32_t f = input[in_idx++];
            f |= (uint32_t(input[in_idx++]) << 8);

            if (accum + f > PROB_TOTAL) throw std::runtime_error("Corrupt table");

            table[sym].freq = f;
            table[sym].cum_freq = accum;
            
            // Fill lookup table
            std::memset(&slot_to_sym[accum], sym, f);
            accum += f;
        }
    } else {
        // --- Dense Mode ---
        if (in_idx + 512 > input.size()) throw std::runtime_error("Input truncated");

        for (int i = 0; i < 256; ++i) {
            uint32_t f = input[in_idx++];
            f |= (uint32_t(input[in_idx++]) << 8);

            if (accum + f > PROB_TOTAL) throw std::runtime_error("Corrupt table");

            table[i].freq = f;
            table[i].cum_freq = accum;

            if (f > 0) {
                std::memset(&slot_to_sym[accum], i, f);
            }
            accum += f;
        }
    }

    if (accum != PROB_TOTAL) throw std::runtime_error("Table sum mismatch");

    // Read Initial State
    if (in_idx + 4 > input.size()) throw std::runtime_error("Input truncated");
    uint32_t state;
    std::memcpy(&state, &input[in_idx], 4);
    in_idx += 4;

    // --- 3. Decoding ---
    for (size_t i = 0; i < original_size; ++i) {
        // Identify symbol from state
        uint32_t slot = state & PROB_MASK;
        uint8_t sym = slot_to_sym[slot];
        output[i] = sym;

        // Update state
        uint32_t freq = table[sym].freq;
        uint32_t cum_freq = table[sym].cum_freq;
        
        // Advance state step 1
        state = freq * (state >> SCALE_BITS) + (slot - cum_freq);

        // Renormalization (Refill)
        while (state < RANS_L) {
            if (in_idx >= input.size()) 
                throw std::runtime_error("Unexpected end of stream");
            state = (state << 8) | input[in_idx++];
        }
    }

    return original_size;
}
