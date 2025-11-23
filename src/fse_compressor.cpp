#include "fse_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <numeric>

namespace {
    constexpr int SCALE_BITS = 12;
    constexpr uint32_t PROB_TOTAL = 1 << SCALE_BITS;
    constexpr uint32_t PROB_MASK = PROB_TOTAL - 1;

    constexpr uint32_t RANS_L = 1 << 15;

    struct SymbolStats {
        uint32_t freq;
        uint32_t cum_freq;
    };
}

size_t FSECompressor::get_max_compressed_size(size_t input_size) const {
    return input_size + 1024 + 8 + 128;
}

size_t FSECompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;
    (void)level;

    std::vector<uint64_t> raw_counts(256, 0);
    for (uint8_t b : input) raw_counts[b]++;

    std::vector<SymbolStats> table(256);
    uint32_t current_sum = 0;
    uint32_t max_symbol_val = 0;
    int max_symbol_idx = -1;

    for (int i = 0; i < 256; ++i) {
        if (raw_counts[i] == 0) {
            table[i].freq = 0;
        } else {
            uint64_t scaled = (raw_counts[i] * PROB_TOTAL) / input.size();
            if (scaled == 0) scaled = 1;
            table[i].freq = static_cast<uint32_t>(scaled);
            current_sum += table[i].freq;
            
            if (table[i].freq > max_symbol_val) {
                max_symbol_val = table[i].freq;
                max_symbol_idx = i;
            }
        }
    }

    if (current_sum != PROB_TOTAL) {
        if (max_symbol_idx != -1) {
            if (current_sum < PROB_TOTAL) 
                table[max_symbol_idx].freq += (PROB_TOTAL - current_sum);
            else 
                table[max_symbol_idx].freq -= (current_sum - PROB_TOTAL);
        } else
            return 0;
    }

    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        table[i].cum_freq = accum;
        accum += table[i].freq;
    }

    std::vector<uint8_t> stream;
    stream.reserve(input.size());
    uint32_t state = RANS_L;

    for (size_t i = input.size(); i > 0; --i) {
        uint8_t sym = input[i - 1];
        uint32_t freq = table[sym].freq;
        uint32_t start = table[sym].cum_freq;

        uint32_t max_val = ((RANS_L >> SCALE_BITS) << 8) * freq;
        while (state >= max_val) {
            stream.push_back(state & 0xFF);
            state >>= 8;
        }
        state = ((state / freq) << SCALE_BITS) + start + (state % freq);
    }

    if (output.size() < 8 + 1024 + 4 + stream.size()) 
        throw std::runtime_error("FSE Output buffer too small");

    size_t out_idx = 0;
    uint64_t os = input.size();
    std::memcpy(&output[out_idx], &os, 8);
    out_idx += 8;

    for (int i = 0; i < 256; ++i) {
        uint32_t f = table[i].freq;
        output[out_idx++] = f & 0xFF;
        output[out_idx++] = (f >> 8) & 0xFF;
        output[out_idx++] = 0; 
        output[out_idx++] = 0;
    }

    std::memcpy(&output[out_idx], &state, 4);
    out_idx += 4;
    for (size_t i = 0; i < stream.size(); ++i)
        output[out_idx++] = stream[stream.size() - 1 - i];
    return out_idx;
}

size_t FSECompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;

    size_t in_idx = 0;

    if (input.size() < 8) throw std::runtime_error("FSE Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, &input[in_idx], 8);
    in_idx += 8;

    if (output.size() < original_size) throw std::runtime_error("FSE Output buffer too small");

    if (input.size() < in_idx + 1024) throw std::runtime_error("FSE Input too small for table");
    std::vector<SymbolStats> table(256);
    std::vector<uint8_t> slot_to_sym(PROB_TOTAL);
    
    uint32_t accum = 0;
    for (int i = 0; i < 256; ++i) {
        uint32_t f = input[in_idx];
        f |= (uint32_t(input[in_idx+1]) << 8);
        in_idx += 4;
        
        table[i].freq = f;
        table[i].cum_freq = accum;

        for (uint32_t j = 0; j < f; ++j) {
            slot_to_sym[accum + j] = static_cast<uint8_t>(i);
        }
        accum += f;
    }
    
    if (accum != PROB_TOTAL) throw std::runtime_error("FSE Corrupt frequency table");

    if (input.size() < in_idx + 4) throw std::runtime_error("FSE Input too small for state");
    uint32_t state;
    std::memcpy(&state, &input[in_idx], 4);
    in_idx += 4;

    for (size_t i = 0; i < original_size; ++i) {
        uint32_t slot = state & PROB_MASK;
        uint8_t sym = slot_to_sym[slot];
        output[i] = sym;
        state = table[sym].freq * (state >> SCALE_BITS) + (slot - table[sym].cum_freq);
        while (state < RANS_L) {
            if (in_idx >= input.size()) {
                 throw std::runtime_error("FSE Unexpected end of stream");
            }
            state = (state << 8) | input[in_idx++];
        }
    }

    return original_size;
}
