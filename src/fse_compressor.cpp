#include "fse_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <numeric>

namespace {
    constexpr int SCALE_BITS = 12;
    constexpr uint32_t PROB_TOTAL = 1 << SCALE_BITS; // 4096
    constexpr uint32_t PROB_MASK = PROB_TOTAL - 1;
    constexpr uint32_t RANS_L = 1 << 15;

    struct SymbolStats {
        uint32_t freq;
        uint32_t cum_freq;
    };
}

size_t FSECompressor::get_max_compressed_size(size_t input_size) const {
    // Header (8) + Table (1024) + State (4) + Raw Data + Safety Margin
    return input_size + 1024 + 512;
}

size_t FSECompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;
    (void)level;

    // 1. Frequency Counting
    std::vector<uint64_t> raw_counts(256, 0);
    for (uint8_t b : input) raw_counts[b]++;

    // 2. Normalization
    std::vector<SymbolStats> table(256);
    uint32_t current_sum = 0;
    int max_symbol_idx = -1;
    uint32_t max_freq = 0;

    // First pass: basic scaling
    for (int i = 0; i < 256; ++i) {
        if (raw_counts[i] == 0) {
            table[i].freq = 0;
        } else {
            // Scale to PROB_TOTAL
            uint64_t scaled = (raw_counts[i] * PROB_TOTAL) / input.size();
            
            // Ensure every present symbol has at least freq 1
            if (scaled == 0) scaled = 1; 
            
            // Cap at PROB_TOTAL (sanity check)
            if (scaled > PROB_TOTAL) scaled = PROB_TOTAL; 
            
            table[i].freq = static_cast<uint32_t>(scaled);
            current_sum += table[i].freq;
            
            // Track most frequent symbol to absorb errors
            if (table[i].freq > max_freq) {
                max_freq = table[i].freq;
                max_symbol_idx = i;
            }
        }
    }

    // 3. Robust Adjustment Loop
    // We must ensure current_sum == PROB_TOTAL (4096)
    if (current_sum < PROB_TOTAL) {
        // Underflow: Add remainder to max symbol
        // If max_symbol_idx is -1 (should be impossible if input not empty), we return 0
        if (max_symbol_idx != -1) {
            table[max_symbol_idx].freq += (PROB_TOTAL - current_sum);
        } else {
            return 0; 
        }
    } else if (current_sum > PROB_TOTAL) {
        // Overflow: We need to reduce total frequency by 'excess'
        uint32_t excess = current_sum - PROB_TOTAL;
        
        // Strategy: First try subtracting from max symbol
        if (max_symbol_idx != -1 && table[max_symbol_idx].freq > excess + 1) {
            table[max_symbol_idx].freq -= excess;
        } else {
            // Fallback: Iteratively reduce frequencies > 1
            // This guarantees we don't set a symbol to 0 (which would break decoding)
            while (excess > 0) {
                bool progress = false;
                for (int i = 0; i < 256 && excess > 0; ++i) {
                    if (table[i].freq > 1) {
                        table[i].freq--;
                        excess--;
                        progress = true;
                    }
                }
                if (!progress) break; // Should not happen given constraints
            }
            
            // If still excess (highly unlikely edge case where all are 1), 
            // force remove from max symbol even if it makes it 1, assuming non-zero constraint held.
            // But the loop above handles >1.
        }
    }

    // Recalculate cum_freq and Verify
    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        table[i].cum_freq = accum;
        accum += table[i].freq;
    }
    
    if (accum != PROB_TOTAL) {
        throw std::runtime_error("FSE Normalization logic failed");
    }

    // 4. Encoding (Reverse)
    std::vector<uint8_t> stream;
    stream.reserve(input.size());
    uint32_t state = RANS_L;

    for (size_t i = input.size(); i > 0; --i) {
        uint8_t sym = input[i - 1];
        uint32_t freq = table[sym].freq;
        uint32_t start = table[sym].cum_freq;

        // Renormalize
        // Range check: if freq is small, state grows fast.
        uint32_t max_val = ((RANS_L >> SCALE_BITS) << 8) * freq;
        while (state >= max_val) {
            stream.push_back(state & 0xFF);
            state >>= 8;
        }
        // Update state
        state = ((state / freq) << SCALE_BITS) + start + (state % freq);
    }

    // 5. Output Writing
    // Format: [8: OriginalSize] [1024: Table] [4: State] [Stream...]
    size_t header_size = 8 + 1024 + 4;
    if (output.size() < header_size + stream.size()) 
        throw std::runtime_error("FSE Output buffer too small");

    size_t out_idx = 0;
    uint64_t os = input.size();
    std::memcpy(&output[out_idx], &os, 8);
    out_idx += 8;

    // Write Table (Fixed 1024 bytes - 4 bytes per symbol)
    // A more compact encoding exists, but this is simple and robust.
    for (int i = 0; i < 256; ++i) {
        uint32_t f = table[i].freq;
        output[out_idx++] = f & 0xFF;
        output[out_idx++] = (f >> 8) & 0xFF;
        output[out_idx++] = 0; 
        output[out_idx++] = 0;
    }

    std::memcpy(&output[out_idx], &state, 4);
    out_idx += 4;
    
    // Write Stream Reversed
    for (size_t i = 0; i < stream.size(); ++i)
        output[out_idx++] = stream[stream.size() - 1 - i];
        
    return out_idx;
}

size_t FSECompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;
    size_t in_idx = 0;

    // 1. Header
    if (input.size() < 8) throw std::runtime_error("FSE Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, &input[in_idx], 8);
    in_idx += 8;

    if (output.size() < original_size) throw std::runtime_error("FSE Output buffer too small");

    // 2. Table Reconstruction
    if (input.size() < in_idx + 1024) throw std::runtime_error("FSE Input too small for table");
    
    std::vector<SymbolStats> table(256);
    std::vector<uint8_t> slot_to_sym(PROB_TOTAL);
    
    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        // Read 4 bytes per entry
        uint32_t f = input[in_idx];
        f |= (uint32_t(input[in_idx+1]) << 8);
        in_idx += 4; 
        
        // CRITICAL: Validate Table Integrity
        // If the table was corrupted, accum + f could exceed 4096, 
        // causing out-of-bounds write to slot_to_sym.
        if (accum + f > PROB_TOTAL) {
            throw std::runtime_error("FSE Corrupt frequency table (overflow)");
        }

        table[i].freq = f;
        table[i].cum_freq = accum;

        if (f > 0) {
            std::memset(&slot_to_sym[accum], i, f);
        }
        accum += f;
    }
    
    if (accum != PROB_TOTAL) throw std::runtime_error("FSE Corrupt frequency table (sum != 4096)");

    // 3. State
    if (input.size() < in_idx + 4) throw std::runtime_error("FSE Input too small for state");
    uint32_t state;
    std::memcpy(&state, &input[in_idx], 4);
    in_idx += 4;

    // 4. Decoding Loop
    for (size_t i = 0; i < original_size; ++i) {
        uint32_t slot = state & PROB_MASK;
        uint8_t sym = slot_to_sym[slot];
        output[i] = sym;

        uint32_t freq = table[sym].freq;
        // Safety: if freq is 0, we have a broken state logic or table
        if (freq == 0) throw std::runtime_error("FSE Decoded zero-frequency symbol");
        
        uint32_t cum_freq = table[sym].cum_freq;
        
        state = freq * (state >> SCALE_BITS) + (slot - cum_freq);

        while (state < RANS_L) {
            if (in_idx >= input.size()) {
                 throw std::runtime_error("FSE Unexpected end of stream");
            }
            state = (state << 8) | input[in_idx++];
        }
    }

    return original_size;
}
