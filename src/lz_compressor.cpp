#include "lz_compressor.h"
#include "fse_compressor.h"
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <array>

namespace {
    // Shared constants
    // FSE works best with byte-aligned data.
    constexpr size_t MATCH_LEN_THRESHOLD = 255; 

    inline size_t get_hash(const uint8_t* p, size_t hash_bits) {
        uint32_t val;
        std::memcpy(&val, p, 4);
        // Knuth Multiplicative Hash
        return (val * 0x9E3779B1) >> (32 - hash_bits);
    }

    // Helper to separate LZ tokens into distinct streams for FSE
    struct StreamSplitter {
        std::vector<uint8_t> controls;
        std::vector<uint8_t> literals;
        std::vector<uint8_t> lengths;
        std::vector<uint8_t> offsets;

        uint8_t current_control = 0;
        int token_count = 0;

        StreamSplitter(size_t reserve_size) {
            // Heuristic reservations
            controls.reserve(reserve_size / 8 + 64);
            literals.reserve(reserve_size / 2 + 64);
            lengths.reserve(reserve_size / 8 + 64);
            offsets.reserve(reserve_size / 4 + 64);
        }

        void add_literal(uint8_t lit) {
            literals.push_back(lit);
            // Bit 0 = Literal
            next_token();
        }

        void add_match(size_t len, size_t dist, size_t min_match_len) {
            // Bit 1 = Match
            current_control |= (1 << token_count);
            
            // 1. Process Length
            size_t len_code = len - min_match_len;
            if (len_code >= MATCH_LEN_THRESHOLD) {
                lengths.push_back(255);
                size_t remaining = len_code - MATCH_LEN_THRESHOLD;
                while (remaining >= 255) {
                    lengths.push_back(255);
                    remaining -= 255;
                }
                lengths.push_back(static_cast<uint8_t>(remaining));
            } else {
                lengths.push_back(static_cast<uint8_t>(len_code));
            }

            // 2. Process Offset (Little Endian 16-bit)
            offsets.push_back(static_cast<uint8_t>(dist & 0xFF));
            offsets.push_back(static_cast<uint8_t>((dist >> 8) & 0xFF));

            next_token();
        }

        void next_token() {
            token_count++;
            if (token_count == 8) flush_control();
        }

        void flush_control() {
            if (token_count > 0) {
                controls.push_back(current_control);
                current_control = 0;
                token_count = 0;
            }
        }

        void finish() {
            flush_control();
        }
    };
}

template <LZMode Mode>
size_t LZCompressor<Mode>::get_max_compressed_size(size_t input_size) const {
    // We have 4 streams. Each FSE stream has a header (frequency table). 
    // For tiny inputs (like 1 byte), the header overhead dominates.
    // 4096 allows ~1KB overhead per stream, which is safe for standard FSE.
    return input_size + (input_size / 4) + 8000;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) return 0;

    // --- CONSTANTS ---
    constexpr size_t WINDOW_SIZE = 65536; 
    constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    constexpr size_t MIN_MATCH_LEN = 3; 
    constexpr size_t HASH_BITS = 16;
    constexpr size_t HASH_SIZE = 1 << HASH_BITS;
    constexpr size_t NICE_MATCH_LEN = 256;

    const uint8_t* ip = input.data();
    const uint8_t* const ip_start = ip;
    const uint8_t* const ip_end = ip + input.size();
    const uint8_t* const ip_limit = ip_end - 5; 

    // --- LZ77 PARSING (Greedy with Full Dict Update) ---
    StreamSplitter streams(input.size());
    std::vector<int32_t> head(HASH_SIZE, -1);
    std::vector<int32_t> prev(WINDOW_SIZE, -1);
    
    // Handle negative levels gracefully if passed
    int eff_level = (level < 1) ? 1 : level;
    uint32_t max_chain = (1u << eff_level);
    if (max_chain > 4096) max_chain = 4096;

    while (ip < ip_end) {
        size_t best_len = 0;
        size_t best_dist = 0;

        if (ip < ip_limit) {
            size_t curr_idx = ip - ip_start;
            size_t hash = get_hash(ip, HASH_BITS);
            int32_t match_idx = head[hash];
            
            prev[curr_idx & WINDOW_MASK] = head[hash];
            head[hash] = static_cast<int32_t>(curr_idx);

            int chain_len = 0;
            while (match_idx != -1 && chain_len < max_chain) {
                size_t dist = curr_idx - match_idx;
                if (dist >= WINDOW_SIZE || dist == 0) break;

                const uint8_t* match_ptr = ip_start + match_idx;
                if (ip[best_len] == match_ptr[best_len] && ip[0] == match_ptr[0]) {
                    size_t len = 0;
                    while (ip + len < ip_end && ip[len] == match_ptr[len]) {
                        len++;
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_dist = dist;
                        if (best_len >= NICE_MATCH_LEN) break;
                    }
                }
                match_idx = prev[match_idx & WINDOW_MASK];
                chain_len++;
            }
        }

        if (best_len >= MIN_MATCH_LEN) {
            streams.add_match(best_len, best_dist, MIN_MATCH_LEN);
            
            const uint8_t* next_ip = ip + best_len;
            ip++; 
            while(ip < next_ip) {
                if (ip < ip_limit) {
                     size_t h = get_hash(ip, HASH_BITS);
                     size_t idx = ip - ip_start;
                     prev[idx & WINDOW_MASK] = head[h];
                     head[h] = static_cast<int32_t>(idx);
                }
                ip++;
            }
        } else {
            streams.add_literal(*ip);
            ip++;
        }
    }
    streams.finish();

    // --- FSE COMPRESSION ---
    
    uint8_t* op = output.data();
    uint8_t* const op_end = op + output.size();

    // 1. Header (Original Size)
    if (op + 8 > op_end) throw std::runtime_error("Output too small for header");
    uint64_t total_size = static_cast<uint64_t>(input.size());
    std::memcpy(op, &total_size, 8);
    op += 8;

    // 2. Stream Sizes Block (4 x 4 bytes)
    if (op + 16 > op_end) throw std::runtime_error("Output too small for stream headers");
    uint8_t* size_ptr = op;
    op += 16; 

    FSECompressor fse;
    
    auto compress_stream = [&](const std::vector<uint8_t>& src, uint8_t*& dest, int idx) {
        if (src.empty()) {
            std::memset(size_ptr + (idx * 4), 0, 4);
            return;
        }
        
        size_t remaining = op_end - dest;
        // Explicitly check if we have practically 0 space (FSE needs margin)
        if (remaining < 64) throw std::runtime_error("Output buffer exhausted before FSE stream");

        // FSE compress
        size_t comp_sz = fse.compress(src, std::span<uint8_t>(dest, remaining), level);
        
        uint32_t sz_u32 = static_cast<uint32_t>(comp_sz);
        std::memcpy(size_ptr + (idx * 4), &sz_u32, 4);
        
        dest += comp_sz;
    };

    compress_stream(streams.controls, op, 0);
    compress_stream(streams.literals, op, 1);
    compress_stream(streams.lengths, op, 2);
    compress_stream(streams.offsets, op, 3);

    return op - output.data();
}

template <LZMode Mode>
size_t LZCompressor<Mode>::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;
    
    const uint8_t* ip = input.data();
    const uint8_t* ip_end = ip + input.size();
    
    if (input.size() < 8) throw std::runtime_error("Input too small");
    uint64_t original_size;
    std::memcpy(&original_size, ip, 8);
    ip += 8;

    if (output.size() < original_size) throw std::runtime_error("Output buffer too small");

    if (ip + 16 > ip_end) throw std::runtime_error("Input too small for stream sizes");
    uint32_t sz_ctrl, sz_lit, sz_len, sz_off;
    std::memcpy(&sz_ctrl, ip + 0, 4);
    std::memcpy(&sz_lit,  ip + 4, 4);
    std::memcpy(&sz_len,  ip + 8, 4);
    std::memcpy(&sz_off,  ip + 12, 4);
    ip += 16;

    FSECompressor fse;
    
    // Temporary buffers for decompression
    // For safety, we allocate somewhat generously. 
    // In a production env, you might calculate exact bounds or reuse buffers.
    auto decompress_stream = [&](size_t comp_sz, std::vector<uint8_t>& dest) {
        if (comp_sz == 0) return;
        if (ip + comp_sz > ip_end) throw std::runtime_error("Truncated FSE stream");
        
        // Resize to a safe upper bound. 
        // Literals can be at most original_size.
        // Controls are original_size / 8 + margin.
        dest.resize(original_size + 256); 
        
        size_t actual = fse.decompress(std::span<const uint8_t>(ip, comp_sz), dest);
        dest.resize(actual);
        ip += comp_sz;
    };

    std::vector<uint8_t> buf_ctrl, buf_lit, buf_len, buf_off;
    // Heuristic pre-allocation to save reallocs
    buf_ctrl.reserve(original_size / 8 + 64);
    buf_lit.reserve(original_size);
    buf_len.reserve(original_size / 8 + 64);
    buf_off.reserve(original_size / 4 + 64);

    decompress_stream(sz_ctrl, buf_ctrl);
    decompress_stream(sz_lit, buf_lit);
    decompress_stream(sz_len, buf_len);
    decompress_stream(sz_off, buf_off);

    // Reconstruct
    uint8_t* op = output.data();
    size_t lit_idx = 0;
    size_t len_idx = 0;
    size_t off_idx = 0;
    size_t bytes_decoded = 0;
    constexpr size_t MIN_MATCH_LEN = 3;

    for (uint8_t control : buf_ctrl) {
        for (int i = 0; i < 8 && bytes_decoded < original_size; ++i) {
            if ((control >> i) & 1) {
                // MATCH
                if (len_idx >= buf_len.size()) throw std::runtime_error("Len buffer underflow");
                
                size_t len_code = buf_len[len_idx++];
                size_t match_len = len_code + MIN_MATCH_LEN;

                if (len_code == 255) {
                    while (len_idx < buf_len.size()) {
                        uint8_t ext = buf_len[len_idx++];
                        match_len += ext;
                        if (ext != 255) break;
                    }
                }

                if (off_idx + 1 >= buf_off.size()) throw std::runtime_error("Off buffer underflow");
                size_t dist = static_cast<size_t>(buf_off[off_idx]) | 
                              (static_cast<size_t>(buf_off[off_idx+1]) << 8);
                off_idx += 2;

                if (dist == 0 || dist > bytes_decoded) throw std::runtime_error("Invalid distance");

                const uint8_t* src = op - dist;
                for (size_t k = 0; k < match_len; ++k) {
                    *op++ = *src++;
                }
                bytes_decoded += match_len;

            } else {
                // LITERAL
                if (lit_idx >= buf_lit.size()) throw std::runtime_error("Lit buffer underflow");
                *op++ = buf_lit[lit_idx++];
                bytes_decoded++;
            }
        }
    }

    return bytes_decoded;
}

// Explicit Instantiation
template class LZCompressor<LZMode::V2B>;
template class LZCompressor<LZMode::V3B>;
