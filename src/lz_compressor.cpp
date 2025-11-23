#include "lz_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <array>

namespace {
    // Must be power of 2 for bitwise optimization
    constexpr size_t WINDOW_SIZE = 4096; 
    constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    
    constexpr size_t HASH_SIZE = 4096;
    constexpr size_t HASH_MASK = HASH_SIZE - 1;
    
    constexpr size_t MIN_MATCH_LEN = 3;
    constexpr size_t MAX_MATCH_LEN = 18; // 3 + 15

    // Inline helper for hashing to avoid function call overhead
    inline size_t get_hash(const uint8_t* p) {
        // Simple XOR hash optimized for speed
        return ((p[0] << 4) ^ (p[1] << 2) ^ p[2]) & HASH_MASK;
    }
}

size_t LZCompressor::get_max_compressed_size(size_t input_size) const {
    // Header (8) + Body + Worst case overhead (~1/8th + padding)
    return input_size + (input_size / 8) + 64;
}

size_t LZCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;
    
    const size_t in_len = input.size();
    const uint8_t* ip = input.data();
    const uint8_t* const ip_end = ip + in_len;
    const uint8_t* const ip_limit = ip_end - MIN_MATCH_LEN;

    uint8_t* op = output.data();
    uint8_t* const op_start = op;
    uint8_t* const op_end = op + output.size();

    // 1. Write Header
    if (output.size() < 8) throw std::runtime_error("Output buffer too small");
    uint64_t size_header = static_cast<uint64_t>(in_len);
    std::memcpy(op, &size_header, 8);
    op += 8;

    // 2. Efficient Hash Tables (Allocated once)
    // head stores the absolute position of the match
    std::vector<int32_t> head(HASH_SIZE, -1);
    // prev stores the previous position for the chain (Circular buffer based on window)
    std::vector<int32_t> prev(WINDOW_SIZE, -1);

    // Chain depth limit based on level
    // Level 0/1 = speed (1 check), Level 9 = max compression
    uint32_t max_chain = (level <= 1) ? 1 : (1u << level);
    if (max_chain > 256) max_chain = 256;

    // 3. Processing Loop
    // Buffers for the current group of 8 items
    uint8_t token_buffer[32]; // Max size (8 * 2 bytes + safety)
    int token_count = 0;
    int token_buf_idx = 0;
    uint8_t control_byte = 0;

    const uint8_t* anchor = ip; // Current processing position

    while (anchor < ip_end) {
        // Prepare to find a match
        size_t best_len = 0;
        size_t best_dist = 0;

        // Only search if we have enough bytes and aren't at the very end
        if (anchor < ip_limit) {
            size_t hash = get_hash(anchor);
            int32_t match_index = head[hash];
            
            // Update Hash Tables
            // Store previous match index in the circular buffer
            prev[(anchor - input.data()) & WINDOW_MASK] = match_index;
            head[hash] = static_cast<int32_t>(anchor - input.data());

            // Search the chain
            int chain_len = 0;
            int32_t current_match = match_index;
            size_t current_pos_idx = anchor - input.data();

            while (current_match != -1 && chain_len < max_chain) {
                size_t dist = current_pos_idx - current_match;
                
                // Distance check:
                // FIX: Changed from (> WINDOW_SIZE) to (>= WINDOW_SIZE).
                // Dist 4096 masks to 0 in 12-bit encoding (4096 & 0xFFF == 0),
                // which causes decompression to fail. We limit dist to 4095.
                if (dist >= WINDOW_SIZE || dist == 0) break;

                // Optimization: Check the match length only if first byte matches
                const uint8_t* match_ptr = input.data() + current_match;
                
                if (anchor[best_len] == match_ptr[best_len]) { // Check byte at current best length first
                    size_t len = 0;
                    // Bounded by max length and input end
                    while (len < MAX_MATCH_LEN && (anchor + len < ip_end) && anchor[len] == match_ptr[len]) {
                        len++;
                    }

                    if (len > best_len && len >= MIN_MATCH_LEN) {
                        best_len = len;
                        best_dist = dist;
                        if (best_len == MAX_MATCH_LEN) break; // Found max possible, stop searching
                    }
                }

                // Move back in chain
                current_match = prev[current_match & WINDOW_MASK];
                chain_len++;
            }
        }

        // Encode Logic
        if (best_len >= MIN_MATCH_LEN) {
            // MATCH FOUND
            // Set bit in control byte (bits are filled LSB to MSB)
            control_byte |= (1 << token_count);
            
            // Format: [4 bits len-3] [12 bits dist]
            // Note: best_dist is guaranteed < 4096 here due to the fix above
            uint16_t token = static_cast<uint16_t>(((best_len - MIN_MATCH_LEN) << 12) | (best_dist & 0xFFF));
            token_buffer[token_buf_idx++] = token & 0xFF;
            token_buffer[token_buf_idx++] = (token >> 8) & 0xFF;

            anchor += best_len;
        } else {
            // LITERAL
            // Control bit is 0 (default), just write byte
            token_buffer[token_buf_idx++] = *anchor;
            anchor++;
        }

        token_count++;

        // Flush group if full
        if (token_count == 8) {
            if (op + 1 + token_buf_idx > op_end) throw std::runtime_error("Output buffer too small");
            
            *op++ = control_byte;
            std::memcpy(op, token_buffer, token_buf_idx);
            op += token_buf_idx;

            // Reset
            control_byte = 0;
            token_count = 0;
            token_buf_idx = 0;
        }
    }

    // Flush remaining tokens
    if (token_count > 0) {
        if (op + 1 + token_buf_idx > op_end) throw std::runtime_error("Output buffer too small");
        *op++ = control_byte;
        std::memcpy(op, token_buffer, token_buf_idx);
        op += token_buf_idx;
    }

    return op - op_start;
}

size_t LZCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;

    const uint8_t* ip = input.data();
    const uint8_t* ip_end = ip + input.size();
    uint8_t* op = output.data();
    uint8_t* op_start = op;
    uint8_t* op_end = op + output.size();

    // 1. Read Original Size
    if (input.size() < 8) throw std::runtime_error("Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, ip, 8);
    ip += 8;

    if (output.size() < original_size) throw std::runtime_error("Output buffer too small");

    // 2. Decompress
    size_t bytes_decoded = 0;
    
    while (ip < ip_end && bytes_decoded < original_size) {
        uint8_t control = *ip++;
        
        for (int i = 0; i < 8 && bytes_decoded < original_size; ++i) {
            // Check for stream end early if bits suggest it
            if (ip >= ip_end && (control >> i) == 0) break; 

            if ((control >> i) & 1) {
                // Match
                if (ip + 2 > ip_end) throw std::runtime_error("Unexpected EOF");
                
                uint16_t token = static_cast<uint16_t>(ip[0] | (ip[1] << 8));
                ip += 2;

                size_t len = (token >> 12) + MIN_MATCH_LEN;
                size_t dist = token & 0xFFF;

                // Safety Check:
                // dist cannot be 0 (LZ77 doesn't allow offset 0)
                // dist cannot be larger than what we've written (can't read before buffer start)
                if (dist == 0 || dist > bytes_decoded) {
                    throw std::runtime_error("Invalid distance");
                }

                // Optimization: Tight copy loop
                uint8_t* src = op - dist;
                
                // Handle overlap (src < op) safely
                while(len--) {
                    *op++ = *src++;
                }
                bytes_decoded = op - op_start;

            } else {
                // Literal
                if (ip >= ip_end) break;
                *op++ = *ip++;
                bytes_decoded++;
            }
        }
    }

    return bytes_decoded;
}
