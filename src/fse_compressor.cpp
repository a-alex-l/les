#include "fse_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <numeric>

namespace {
    // rANS Constants
    constexpr int SCALE_BITS = 12; // Target total frequency = 4096
    constexpr uint32_t PROB_TOTAL = 1 << SCALE_BITS;
    constexpr uint32_t PROB_MASK = PROB_TOTAL - 1;

    constexpr uint32_t RANS_L = 1 << 15;  // Lower bound for state (renormalization limit)

    struct SymbolStats {
        uint32_t freq;
        uint32_t cum_freq;
    };
}

size_t FSECompressor::get_max_compressed_size(size_t input_size) const {
    // Overhead:
    // 8 bytes: Original Size
    // 1024 bytes: Frequency Table (256 * 4)
    // 4 bytes: Final State
    // Input size: The compressed stream itself
    // + Padding/Margin
    return input_size + 1024 + 8 + 128;
}

size_t FSECompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;
    (void)level;

    // 1. Calculate Raw Frequencies
    std::vector<uint64_t> raw_counts(256, 0);
    for (uint8_t b : input) raw_counts[b]++;

    // 2. Normalize Frequencies to sum to PROB_TOTAL (4096)
    std::vector<SymbolStats> table(256);
    uint32_t current_sum = 0;
    uint32_t max_symbol_val = 0;
    int max_symbol_idx = -1;

    for (int i = 0; i < 256; ++i) {
        if (raw_counts[i] == 0) {
            table[i].freq = 0;
        } else {
            // Scale: (count * 4096) / total
            uint64_t scaled = (raw_counts[i] * PROB_TOTAL) / input.size();
            if (scaled == 0) scaled = 1; // Ensure seen symbols have at least freq 1
            table[i].freq = static_cast<uint32_t>(scaled);
            current_sum += table[i].freq;
            
            if (table[i].freq > max_symbol_val) {
                max_symbol_val = table[i].freq;
                max_symbol_idx = i;
            }
        }
    }

    // Adjust sum to exactly PROB_TOTAL by tweaking the most frequent symbol
    if (current_sum != PROB_TOTAL) {
        if (max_symbol_idx != -1) {
            if (current_sum < PROB_TOTAL) 
                table[max_symbol_idx].freq += (PROB_TOTAL - current_sum);
            else 
                table[max_symbol_idx].freq -= (current_sum - PROB_TOTAL);
        } else {
            // Should not happen for non-empty input unless logic error
            return 0; 
        }
    }

    // Build Cumulative Frequencies
    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        table[i].cum_freq = accum;
        accum += table[i].freq;
    }

    // 3. rANS Encoding (Backwards)
    std::vector<uint8_t> stream;
    stream.reserve(input.size());

    uint32_t state = RANS_L; // Initial state

    for (size_t i = input.size(); i > 0; --i) {
        uint8_t sym = input[i - 1];
        uint32_t freq = table[sym].freq;
        uint32_t start = table[sym].cum_freq;

        // Renormalize: output byte if state is too large.
        // Limit calculation: ensure state fits in valid range [L, H] after update.
        // Standard rANS limit: ((L >> Scale) << 8) * freq
        // RANS_L = 2^15, Scale = 12. (RANS_L >> Scale) = 8.
        // Limit = (8 << 8) * freq = 2048 * freq.
        uint32_t max_val = ((RANS_L >> SCALE_BITS) << 8) * freq;
        
        while (state >= max_val) {
            stream.push_back(state & 0xFF);
            state >>= 8;
        }

        // Update State
        // x = ((x / freq) * PROB_TOTAL) + cum_freq + (x % freq)
        state = ((state / freq) << SCALE_BITS) + start + (state % freq);
    }

    // 4. Write Output
    if (output.size() < 8 + 1024 + 4 + stream.size()) 
        throw std::runtime_error("FSE Output buffer too small");

    size_t out_idx = 0;

    // Original Size (8 bytes)
    uint64_t os = input.size();
    std::memcpy(&output[out_idx], &os, 8);
    out_idx += 8;

    // Frequency Table (Normalized) - 1024 bytes
    // Important: Write the *table[i].freq*, NOT raw_counts!
    for (int i = 0; i < 256; ++i) {
        uint32_t f = table[i].freq;
        output[out_idx++] = f & 0xFF;
        output[out_idx++] = (f >> 8) & 0xFF;
        output[out_idx++] = 0; 
        output[out_idx++] = 0;
    }

    // Final State (4 bytes)
    std::memcpy(&output[out_idx], &state, 4);
    out_idx += 4;

    // Stream (Reverse written to be read sequentially)
    for (size_t i = 0; i < stream.size(); ++i) {
        output[out_idx++] = stream[stream.size() - 1 - i];
    }

    return out_idx;
}

size_t FSECompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;

    size_t in_idx = 0;

    // 1. Read Original Size
    if (input.size() < 8) throw std::runtime_error("FSE Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, &input[in_idx], 8);
    in_idx += 8;

    if (output.size() < original_size) throw std::runtime_error("FSE Output buffer too small");

    // 2. Read Frequencies
    if (input.size() < in_idx + 1024) throw std::runtime_error("FSE Input too small for table");
    std::vector<SymbolStats> table(256);
    std::vector<uint8_t> slot_to_sym(PROB_TOTAL);
    
    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        // Read 4 bytes per freq
        uint32_t f = input[in_idx];
        f |= (uint32_t(input[in_idx+1]) << 8);
        in_idx += 4; // Skip padding
        
        table[i].freq = f;
        table[i].cum_freq = accum;
        
        // Fill lookup table
        for (uint32_t j = 0; j < f; ++j) {
            slot_to_sym[accum + j] = static_cast<uint8_t>(i);
        }
        accum += f;
    }
    
    if (accum != PROB_TOTAL) throw std::runtime_error("FSE Corrupt frequency table");

    // 3. Read Initial State
    if (input.size() < in_idx + 4) throw std::runtime_error("FSE Input too small for state");
    uint32_t state;
    std::memcpy(&state, &input[in_idx], 4);
    in_idx += 4;

    // 4. Decode (Forward)
    for (size_t i = 0; i < original_size; ++i) {
        // Get slot
        uint32_t slot = state & PROB_MASK;
        
        // Determine symbol
        uint8_t sym = slot_to_sym[slot];
        output[i] = sym;

        // Update State
        state = table[sym].freq * (state >> SCALE_BITS) + (slot - table[sym].cum_freq);

        // Renormalize
        while (state < RANS_L) {
            if (in_idx >= input.size()) {
                 throw std::runtime_error("FSE Unexpected end of stream");
            }
            state = (state << 8) | input[in_idx++];
        }
    }

    return original_size;
}
