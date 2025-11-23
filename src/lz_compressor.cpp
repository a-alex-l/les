#include "lz_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace {
    // Shared constants
    constexpr size_t MATCH_LEN_THRESHOLD_V2 = 15;  // 4 bits (max 15)
    constexpr size_t MATCH_LEN_THRESHOLD_V3 = 255; // 8 bits (max 255)

    inline size_t get_hash(const uint8_t* p, size_t hash_bits) {
        uint32_t val;
        // Read 4 bytes for better hashing
        std::memcpy(&val, p, 4);
        return (val * 0x1e35a7bd) >> (32 - hash_bits);
    }
}

template <LZMode Mode>
size_t LZCompressor<Mode>::get_max_compressed_size(size_t input_size) const {
    return input_size + (input_size / 8) + 128;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;
    
    // --- TEMPLATE CONSTANTS ---
    constexpr size_t WINDOW_SIZE = (Mode == LZMode::V2B) ? 4096 : 65536;
    constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    constexpr size_t MIN_MATCH_LEN = (Mode == LZMode::V2B) ? 3 : 4;
    // 3B mode uses larger hash table to reduce collisions
    constexpr size_t HASH_BITS = (Mode == LZMode::V2B) ? 12 : 16;
    constexpr size_t HASH_SIZE = 1 << HASH_BITS;
    
    // Optimization heuristics
    constexpr size_t NICE_MATCH_LEN = (Mode == LZMode::V2B) ? 32 : 128;

    const size_t in_len = input.size();
    const uint8_t* ip = input.data();
    const uint8_t* const ip_end = ip + in_len;
    const uint8_t* const ip_limit = ip_end - 5; 

    uint8_t* op = output.data();
    uint8_t* const op_start = op;
    uint8_t* const op_end = op + output.size();

    // 1. Write Header
    if (output.size() < 8) throw std::runtime_error("Output buffer too small");
    uint64_t size_header = static_cast<uint64_t>(in_len);
    std::memcpy(op, &size_header, 8);
    op += 8;

    // 2. Hash Tables
    std::vector<int32_t> head(HASH_SIZE, -1);
    std::vector<int32_t> prev(WINDOW_SIZE, -1);

    uint32_t max_chain = (level <= 1) ? 2 : (1u << level);
    if (max_chain > 256) max_chain = 256;

    std::vector<uint8_t> token_buffer;
    token_buffer.reserve(512); 

    int token_count = 0;
    uint8_t control_byte = 0;
    const uint8_t* anchor = ip; 

    while (anchor < ip_end) {
        size_t best_len = 0;
        size_t best_dist = 0;

        if (anchor < ip_limit) {
            size_t hash = get_hash(anchor, HASH_BITS);
            int32_t match_index = head[hash];
            
            prev[(anchor - input.data()) & WINDOW_MASK] = match_index;
            head[hash] = static_cast<int32_t>(anchor - input.data());

            int chain_len = 0;
            int32_t current_match = match_index;
            size_t current_pos_idx = anchor - input.data();

            while (current_match != -1 && chain_len < max_chain) {
                size_t dist = current_pos_idx - current_match;
                if (dist >= WINDOW_SIZE || dist == 0) break;

                const uint8_t* match_ptr = input.data() + current_match;
                
                // Quick check at min length + start
                if (anchor[MIN_MATCH_LEN-1] == match_ptr[MIN_MATCH_LEN-1] && 
                    anchor[0] == match_ptr[0]) {
                    
                    size_t len = 0;
                    while ((anchor + len < ip_end) && anchor[len] == match_ptr[len]) {
                        len++;
                    }

                    if (len > best_len && len >= MIN_MATCH_LEN) {
                        best_len = len;
                        best_dist = dist;
                        if (best_len >= NICE_MATCH_LEN) break; 
                    }
                }
                current_match = prev[current_match & WINDOW_MASK];
                chain_len++;
            }
        }

        if (best_len >= MIN_MATCH_LEN) {
            // MATCH
            control_byte |= (1 << token_count);
            
            if constexpr (Mode == LZMode::V2B) {
                // --- V2B FORMAT: [4:len][12:dist] ---
                size_t len_code = best_len - MIN_MATCH_LEN;
                if (len_code > MATCH_LEN_THRESHOLD_V2) len_code = MATCH_LEN_THRESHOLD_V2;

                uint16_t token = static_cast<uint16_t>((len_code << 12) | (best_dist & 0xFFF));
                token_buffer.push_back(token & 0xFF);
                token_buffer.push_back((token >> 8) & 0xFF);

                if (len_code == MATCH_LEN_THRESHOLD_V2) {
                    size_t remaining = (best_len - MIN_MATCH_LEN) - MATCH_LEN_THRESHOLD_V2;
                    while (remaining >= 255) { token_buffer.push_back(255); remaining -= 255; }
                    token_buffer.push_back(static_cast<uint8_t>(remaining));
                }

            } else {
                // --- V3B FORMAT: [8:len][16:dist] ---
                size_t len_code = best_len - MIN_MATCH_LEN;
                if (len_code > MATCH_LEN_THRESHOLD_V3) len_code = MATCH_LEN_THRESHOLD_V3;

                token_buffer.push_back(static_cast<uint8_t>(len_code));
                token_buffer.push_back(static_cast<uint8_t>(best_dist & 0xFF));
                token_buffer.push_back(static_cast<uint8_t>((best_dist >> 8) & 0xFF));

                if (len_code == MATCH_LEN_THRESHOLD_V3) {
                    size_t remaining = (best_len - MIN_MATCH_LEN) - MATCH_LEN_THRESHOLD_V3;
                    while (remaining >= 255) { token_buffer.push_back(255); remaining -= 255; }
                    token_buffer.push_back(static_cast<uint8_t>(remaining));
                }
            }

            anchor += best_len;
        } else {
            // LITERAL
            token_buffer.push_back(*anchor);
            anchor++;
        }

        token_count++;

        if (token_count == 8) {
            if (op + 1 + token_buffer.size() > op_end) throw std::runtime_error("Output buffer too small");
            *op++ = control_byte;
            std::memcpy(op, token_buffer.data(), token_buffer.size());
            op += token_buffer.size();

            control_byte = 0;
            token_count = 0;
            token_buffer.clear();
        }
    }

    if (token_count > 0) {
        if (op + 1 + token_buffer.size() > op_end) throw std::runtime_error("Output buffer too small");
        *op++ = control_byte;
        std::memcpy(op, token_buffer.data(), token_buffer.size());
        op += token_buffer.size();
    }

    return op - op_start;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;
    
    constexpr size_t MIN_MATCH_LEN = (Mode == LZMode::V2B) ? 3 : 4;

    const uint8_t* ip = input.data();
    const uint8_t* ip_end = ip + input.size();
    uint8_t* op = output.data();
    uint8_t* op_start = op;
    uint8_t* op_end = op + output.size();

    if (input.size() < 8) throw std::runtime_error("Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, ip, 8);
    ip += 8;

    if (output.size() < original_size) throw std::runtime_error("Output buffer too small");

    size_t bytes_decoded = 0;
    
    while (ip < ip_end && bytes_decoded < original_size) {
        uint8_t control = *ip++;
        
        for (int i = 0; i < 8 && bytes_decoded < original_size; ++i) {
            if ((control >> i) & 1) {
                // MATCH
                size_t match_len, dist;

                if constexpr (Mode == LZMode::V2B) {
                    if (ip + 2 > ip_end) throw std::runtime_error("Unexpected EOF");
                    uint16_t token = static_cast<uint16_t>(ip[0] | (ip[1] << 8));
                    ip += 2;
                    size_t len_code = (token >> 12);
                    dist = token & 0xFFF;
                    match_len = len_code + MIN_MATCH_LEN;

                    if (len_code == MATCH_LEN_THRESHOLD_V2) {
                        if (ip >= ip_end) throw std::runtime_error("Unexpected EOF");
                        uint8_t ext = *ip++;
                        match_len += ext;
                        while (ext == 255) {
                             if (ip >= ip_end) throw std::runtime_error("Unexpected EOF");
                             ext = *ip++;
                             match_len += ext;
                        }
                    }
                } else {
                    if (ip + 3 > ip_end) throw std::runtime_error("Unexpected EOF");
                    size_t len_code = ip[0];
                    dist = static_cast<size_t>(ip[1]) | (static_cast<size_t>(ip[2]) << 8);
                    ip += 3;
                    match_len = len_code + MIN_MATCH_LEN;

                    if (len_code == MATCH_LEN_THRESHOLD_V3) {
                         if (ip >= ip_end) throw std::runtime_error("Unexpected EOF");
                        uint8_t ext = *ip++;
                        match_len += ext;
                        while (ext == 255) {
                             if (ip >= ip_end) throw std::runtime_error("Unexpected EOF");
                             ext = *ip++;
                             match_len += ext;
                        }
                    }
                }

                if (dist == 0 || dist > bytes_decoded) throw std::runtime_error("Invalid distance");

                uint8_t* src = op - dist;
                if (op + match_len > op_end) throw std::runtime_error("Output overrun");
                
                for(size_t j=0; j<match_len; ++j) { *op++ = *src++; }
                bytes_decoded += match_len;

            } else {
                if (ip >= ip_end) break; 
                *op++ = *ip++;
                bytes_decoded++;
            }
        }
    }
    return bytes_decoded;
}

// Explicit Instantiation
template class LZCompressor<LZMode::V2B>;
template class LZCompressor<LZMode::V3B>;