#include "lz_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <array>

namespace {
    // Shared constants
    constexpr size_t MATCH_LEN_THRESHOLD_V2 = 15;  
    constexpr size_t MATCH_LEN_THRESHOLD_V3 = 255; 

    // Knuth Multiplicative Hash for better distribution
    inline size_t get_hash(const uint8_t* p, size_t hash_bits) {
        uint32_t val;
        std::memcpy(&val, p, 4);
        return (val * 0x9E3779B1) >> (32 - hash_bits);
    }
}

// Helper class to handle the bit-packing/token writing clean and fast
struct TokenWriter {
    std::vector<uint8_t> buffer;
    uint8_t control_byte = 0;
    int token_count = 0;
    uint8_t*& op;
    uint8_t* const op_end;

    TokenWriter(uint8_t*& output_ptr, uint8_t* output_end) 
        : op(output_ptr), op_end(output_end) {
        buffer.reserve(128);
    }

    void flush() {
        if (token_count > 0) {
            if (op + 1 + buffer.size() > op_end) throw std::runtime_error("Output buffer too small");
            *op++ = control_byte;
            std::memcpy(op, buffer.data(), buffer.size());
            op += buffer.size();
            
            control_byte = 0;
            token_count = 0;
            buffer.clear();
        }
    }

    void add_literal(uint8_t lit) {
        // Literal bit is 0 (implicitly handled by not setting the bit)
        buffer.push_back(lit);
        token_count++;
        check_flush();
    }

    template <LZMode Mode>
    void add_match(size_t len, size_t dist) {
        // Set bit to 1 for match
        control_byte |= (1 << token_count);
        
        constexpr size_t MIN_LEN = (Mode == LZMode::V2B) ? 3 : 4;
        size_t len_code = len - MIN_LEN;

        if constexpr (Mode == LZMode::V2B) {
            // V2B: [4:len][12:dist]
            size_t write_len = (len_code > MATCH_LEN_THRESHOLD_V2) ? MATCH_LEN_THRESHOLD_V2 : len_code;
            uint16_t token = static_cast<uint16_t>((write_len << 12) | (dist & 0xFFF));
            buffer.push_back(token & 0xFF);
            buffer.push_back((token >> 8) & 0xFF);

            if (write_len == MATCH_LEN_THRESHOLD_V2) {
                size_t remaining = len_code - MATCH_LEN_THRESHOLD_V2;
                while (remaining >= 255) { buffer.push_back(255); remaining -= 255; }
                buffer.push_back(static_cast<uint8_t>(remaining));
            }
        } else {
            // V3B: [8:len][16:dist]
            size_t write_len = (len_code > MATCH_LEN_THRESHOLD_V3) ? MATCH_LEN_THRESHOLD_V3 : len_code;
            buffer.push_back(static_cast<uint8_t>(write_len));
            buffer.push_back(static_cast<uint8_t>(dist & 0xFF));
            buffer.push_back(static_cast<uint8_t>((dist >> 8) & 0xFF));

            if (write_len == MATCH_LEN_THRESHOLD_V3) {
                size_t remaining = len_code - MATCH_LEN_THRESHOLD_V3;
                while (remaining >= 255) { buffer.push_back(255); remaining -= 255; }
                buffer.push_back(static_cast<uint8_t>(remaining));
            }
        }
        token_count++;
        check_flush();
    }

    void check_flush() {
        if (token_count == 8) flush();
    }
};

template <LZMode Mode>
size_t LZCompressor<Mode>::get_max_compressed_size(size_t input_size) const {
    return input_size + (input_size / 8) + 256;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;

    // --- CONFIGURATION ---
    constexpr size_t WINDOW_SIZE = (Mode == LZMode::V2B) ? 4096 : 65536;
    constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    constexpr size_t MIN_MATCH_LEN = (Mode == LZMode::V2B) ? 3 : 4;
    // Increased V2B hash bits slightly to reduce collisions
    constexpr size_t HASH_BITS = (Mode == LZMode::V2B) ? 13 : 16; 
    constexpr size_t HASH_SIZE = 1 << HASH_BITS;
    constexpr size_t NICE_MATCH_LEN = (Mode == LZMode::V2B) ? 32 : 256;

    const uint8_t* ip = input.data();
    const uint8_t* const ip_start = ip;
    const uint8_t* const ip_end = ip + input.size();
    const uint8_t* const ip_limit = ip_end - 5; 

    uint8_t* op = output.data();
    uint8_t* const op_end = op + output.size();
    uint8_t* const op_start = op;

    // 1. Write Header
    if (output.size() < 8) throw std::runtime_error("Output buffer too small");
    uint64_t size_header = static_cast<uint64_t>(input.size());
    std::memcpy(op, &size_header, 8);
    op += 8;

    // 2. Structures
    std::vector<int32_t> head(HASH_SIZE, -1);
    std::vector<int32_t> prev(WINDOW_SIZE, -1);
    
    // Chain depth based on level
    uint32_t max_chain = (level < 1) ? 4 : (8u << level); 
    if (max_chain > 2048) max_chain = 2048; 

    TokenWriter writer(op, op_end);

    // --- HELPER: Update Hash ---
    // Adds the string starting at p to the dictionary
    auto update_hash = [&](const uint8_t* p) {
        if (p >= ip_limit) return;
        size_t h = get_hash(p, HASH_BITS);
        size_t idx = p - ip_start;
        prev[idx & WINDOW_MASK] = head[h];
        head[h] = static_cast<int32_t>(idx);
    };

    // --- HELPER: Find Best Match ---
    auto find_match = [&](const uint8_t* curr_ip, size_t& out_dist) -> size_t {
        if (curr_ip >= ip_limit) return 0;
        
        size_t best_len = 0;
        size_t curr_idx = curr_ip - ip_start;
        size_t hash = get_hash(curr_ip, HASH_BITS);
        int32_t match_idx = head[hash];
        
        int chain_len = 0;
        
        while (match_idx != -1 && chain_len < max_chain) {
            size_t dist = curr_idx - match_idx;
            if (dist >= WINDOW_SIZE || dist == 0) break;

            const uint8_t* match_ptr = ip_start + match_idx;

            // Optimization: Check match end first, then start
            if (curr_ip[best_len] == match_ptr[best_len] && 
                curr_ip[0] == match_ptr[0]) {
                
                size_t len = 0;
                while (curr_ip + len < ip_end && curr_ip[len] == match_ptr[len]) {
                    len++;
                }

                if (len > best_len) {
                    best_len = len;
                    out_dist = dist;
                    if (len >= NICE_MATCH_LEN) break;
                }
            }

            match_idx = prev[match_idx & WINDOW_MASK];
            chain_len++;
        }
        
        if (best_len < MIN_MATCH_LEN) return 0;
        return best_len;
    };

    // --- MAIN LOOP (GREEDY) ---
    while (ip < ip_end) {
        // 1. Find match at current position
        size_t dist = 0;
        size_t len = find_match(ip, dist);

        // 2. Emit Token
        if (len >= MIN_MATCH_LEN) {
            // Match found
            writer.add_match<Mode>(len, dist);
            
            // CRITICAL IMPROVEMENT:
            // Even though we skip 'len' bytes in input, we must update the hash table
            // for all of them. This allows future matches to point into the middle 
            // of this sequence.
            for (size_t i = 0; i < len; ++i) {
                update_hash(ip + i);
            }
            ip += len;
        } else {
            // Literal
            writer.add_literal(*ip);
            update_hash(ip);
            ip++;
        }
    }

    writer.flush();
    return op - op_start;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    // Decompressor logic is standard and matches the format
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

                const uint8_t* src = op - dist;
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
