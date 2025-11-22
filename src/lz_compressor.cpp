#include "lz_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace {
    // LZ77 Constants
    constexpr size_t MIN_MATCH_LEN = 3;
    constexpr size_t MAX_MATCH_LEN = 18; // 3 + 15 (4 bits)
    // Increased Window/Hash size slightly for better real-world performance,
    // but kept within reasonable memory limits.
    constexpr size_t WINDOW_SIZE = 4096; 
    constexpr size_t HASH_SIZE = 4096; 
}

size_t LZCompressor::get_max_compressed_size(size_t input_size) const {
    // Worst case: 1 control byte for every 8 literals + the literals themselves.
    // + Header overhead (original size).
    // + Safety margin.
    return input_size + (input_size / 8) + 64;
}

size_t LZCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;

    // 1. Configuration based on Level
    // Level 1 -> 1 check
    // Level 5 -> 16 checks
    // Level 9 -> 256 checks
    // This allows trading CPU time for finding better matches.
    int max_chain_depth = (level <= 1) ? 1 : (1 << (level - 1));
    if (max_chain_depth > 256) max_chain_depth = 256;

    std::vector<uint8_t> buffer;
    // Reserve to prevent reallocations
    buffer.reserve(input.size());

    // 2. Write Original Size Header (8 bytes)
    uint64_t original_size = input.size();
    const uint8_t* size_ptr = reinterpret_cast<const uint8_t*>(&original_size);
    buffer.insert(buffer.end(), size_ptr, size_ptr + 8);

    // 3. Initialize Hash Table and Chain
    // head: maps hash -> index of most recent occurrence
    // prev: maps index -> index of previous occurrence (linked list)
    std::vector<int> head(HASH_SIZE, -1);
    std::vector<int> prev(input.size(), -1);

    size_t ip = 0; // Input pointer

    // Temporary buffers for the Control Byte group
    std::vector<uint8_t> token_buf;
    token_buf.reserve(16);
    std::vector<bool> flags; // false = literal, true = match
    flags.reserve(8);

    // Helper to flush the group of 8 items
    auto flush_tokens = [&](std::vector<uint8_t>& dest) {
        if (flags.empty()) return;
        
        uint8_t control = 0;
        for (size_t i = 0; i < flags.size(); ++i) {
            if (flags[i]) control |= (1 << i);
        }
        dest.push_back(control);
        dest.insert(dest.end(), token_buf.begin(), token_buf.end());
        
        token_buf.clear();
        flags.clear();
    };

    while (ip < input.size()) {
        size_t best_len = 0;
        size_t best_dist = 0;

        // Only look for matches if enough bytes remain
        if (ip + MIN_MATCH_LEN <= input.size()) {
            // Simple Hash: XOR 3 bytes
            uint16_t hash = ((input[ip] << 4) ^ (input[ip + 1] << 2) ^ input[ip + 2]) % HASH_SIZE;
            
            int match_index = head[hash];
            
            // Update Hash Chain
            prev[ip] = match_index;
            head[hash] = static_cast<int>(ip);

            // --- Chain Traversal (The "Level" Logic) ---
            int chain_len = 0;
            int current_match = match_index;

            while (current_match != -1 && chain_len < max_chain_depth) {
                // Check distance
                size_t dist = ip - current_match;
                if (dist > WINDOW_SIZE) {
                    break; // Match too far away, and chain is ordered by time, so others are further
                }

                // Check if this match is actually useful
                // (Optimization: check last byte first to fail fast)
                if (input[current_match + best_len] == input[ip + best_len]) {
                    size_t len = 0;
                    while (len < MAX_MATCH_LEN && 
                           (ip + len < input.size()) && 
                           (input[ip + len] == input[current_match + len])) {
                        len++;
                    }

                    if (len > best_len && len >= MIN_MATCH_LEN) {
                        best_len = len;
                        best_dist = dist;
                        
                        // Heuristic: If we found the max possible length, stop searching.
                        if (best_len == MAX_MATCH_LEN) break;
                    }
                }

                // Move to previous match in chain
                current_match = prev[current_match];
                chain_len++;
            }
        }

        if (best_len >= MIN_MATCH_LEN) {
            // Encode Match
            // [4 bits length-3] [12 bits offset]
            uint16_t token = static_cast<uint16_t>(((best_len - MIN_MATCH_LEN) << 12) | (best_dist & 0xFFF));
            token_buf.push_back(token & 0xFF);
            token_buf.push_back((token >> 8) & 0xFF);
            flags.push_back(true);
            
            // Advance input
            // Note: We should technically update the hash chain for the skipped bytes 
            // to maintain optimal compression, but for speed, we skip them here.
            // (Updating them would make this LZ77 "Parse" complete but slower).
            ip += best_len;
        } else {
            // Encode Literal
            token_buf.push_back(input[ip]);
            flags.push_back(false);
            ip++;
        }

        if (flags.size() == 8) {
            flush_tokens(buffer);
        }
    }

    flush_tokens(buffer);

    if (buffer.size() > output.size()) {
        throw std::runtime_error("LZ Output buffer too small");
    }

    std::memcpy(output.data(), buffer.data(), buffer.size());
    return buffer.size();
}

size_t LZCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;

    size_t in_idx = 0;
    size_t out_idx = 0;

    // 1. Read Original Size
    if (input.size() < 8) throw std::runtime_error("LZ Input too small for header");
    uint64_t original_size = *reinterpret_cast<const uint64_t*>(&input[in_idx]);
    in_idx += 8;

    if (output.size() < original_size) throw std::runtime_error("LZ Output buffer too small");

    // 2. Decompress Stream
    while (in_idx < input.size() && out_idx < original_size) {
        uint8_t control = input[in_idx++];
        
        for (int i = 0; i < 8 && out_idx < original_size; ++i) {
            // Check if we ran out of input bits unexpectedly (allowed if last block matches exactly)
            if (in_idx >= input.size() && (control >> i) != 0) break;

            bool is_match = (control >> i) & 1;

            if (is_match) {
                if (in_idx + 2 > input.size()) throw std::runtime_error("LZ Unexpected end of stream in match");
                
                uint16_t raw = input[in_idx] | (input[in_idx+1] << 8);
                in_idx += 2;

                size_t len = (raw >> 12) + MIN_MATCH_LEN;
                size_t dist = raw & 0xFFF;

                if (dist == 0 || dist > out_idx) {
                     throw std::runtime_error("LZ Invalid back reference distance");
                }

                // Copy memory (cannot use memcpy due to overlapping ranges logic in LZ)
                size_t src_idx = out_idx - dist;
                for (size_t l = 0; l < len; ++l) {
                    if (out_idx >= output.size()) throw std::runtime_error("LZ Output overflow");
                    output[out_idx++] = output[src_idx + l];
                }
            } else {
                if (in_idx >= input.size()) break; 
                output[out_idx++] = input[in_idx++];
            }
        }
    }

    return out_idx;
}
