#include "fuzzy_lz_compressor.h"
#include "huffman_compressor.h"
#include "fse_compressor.h"

#include <vector>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <immintrin.h> // For SSE/AVX

namespace {

// --- Configuration ---
constexpr int MIN_MATCH = 16;
constexpr int MAX_DIST = 65535;       
constexpr int HASH_BITS = 15;         
constexpr int HASH_SIZE = 1 << HASH_BITS;
constexpr int MAX_DIFFS = 3;          // REDUCED: Stricter error tolerance (was 4)
constexpr int MAX_MATCH_LEN = 126;    // SAFE LIMIT

// --- SIMD Helpers ---
#if defined(__GNUC__) || defined(__clang__)
    #define TARGET_SIMD __attribute__((target("sse4.2,popcnt")))
#else
    #define TARGET_SIMD
#endif

// Load 16 bytes unaligned
TARGET_SIMD
inline __m128i load128(const uint8_t* p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
}

// 64-bit Hash Multiplier (Knuth)
inline uint32_t hash_func(uint64_t val) {
    return (uint32_t)((val * 11400714819323198485llu) >> (64 - HASH_BITS));
}

// Split 16 bytes into Even/Odd 64-bit integers
TARGET_SIMD
inline void split_even_odd(__m128i val, uint64_t& even, uint64_t& odd) {
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,
        1, 3, 5, 7, 9, 11, 13, 15
    );
    __m128i shuffled = _mm_shuffle_epi8(val, mask);
    even = (uint64_t)_mm_cvtsi128_si64(shuffled);
    odd = (uint64_t)_mm_extract_epi64(shuffled, 1);
}

// Calculate Hamming Distance (Population Count of XOR)
TARGET_SIMD
inline int get_pop_diff(__m128i a, __m128i b) {
    __m128i x = _mm_xor_si128(a, b);
    uint64_t v0 = (uint64_t)_mm_cvtsi128_si64(x);
    uint64_t v1 = (uint64_t)_mm_extract_epi64(x, 1);
    return (int)(_mm_popcnt_u64(v0) + _mm_popcnt_u64(v1));
}

} // namespace

size_t FuzzyLZCompressor::get_max_compressed_size(size_t input_size) const {
    HuffmanCompressor huff;
    FSECompressor fse;
    return huff.get_max_compressed_size(input_size) + fse.get_max_compressed_size(input_size / 8) + 64;
}

TARGET_SIMD
size_t FuzzyLZCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    const size_t input_size = input.size();
    if (input_size == 0) return 0;

    const uint8_t* src = input.data();
    size_t ip = 0;
    size_t anchor = 0;

    std::vector<uint8_t> main_stream;
    std::vector<uint8_t> delta_stream;
    main_stream.reserve(input_size);
    delta_stream.reserve(input_size / 16);

    std::vector<int> table_even(HASH_SIZE, -1);
    std::vector<int> table_odd(HASH_SIZE, -1);

    if (input_size >= MIN_MATCH) {
        while (ip < input_size - MIN_MATCH) {
            __m128i val_curr = load128(src + ip);
            uint64_t u_even, u_odd;
            split_even_odd(val_curr, u_even, u_odd);

            uint32_t h_even = hash_func(u_even);
            uint32_t h_odd = hash_func(u_odd);

            int ref_e = table_even[h_even];
            int ref_o = table_odd[h_odd];
            
            table_even[h_even] = (int)ip;
            table_odd[h_odd] = (int)ip;

            int best_ref = -1;
            int min_cost = MAX_DIFFS + 1;

            if (ref_e != -1) {
                size_t dist = ip - (size_t)ref_e;
                if (dist < MAX_DIST) {
                    __m128i val_ref = load128(src + ref_e);
                    int cost = get_pop_diff(val_curr, val_ref);
                    if (cost < min_cost) {
                        min_cost = cost;
                        best_ref = ref_e;
                    }
                }
            }

            if (min_cost > 0 && ref_o != -1 && ref_o != ref_e) {
                size_t dist = ip - (size_t)ref_o;
                if (dist < MAX_DIST) {
                    __m128i val_ref = load128(src + ref_o);
                    int cost = get_pop_diff(val_curr, val_ref);
                    if (cost < min_cost) {
                        min_cost = cost;
                        best_ref = ref_o;
                    }
                }
            }

            if (best_ref != -1) {
                // A. Emit Literals
                size_t lit_len = ip - anchor;
                while (lit_len > 0) {
                    size_t chunk = std::min(lit_len, (size_t)127);
                    if (chunk == 127) main_stream.push_back(0x7F);
                    else main_stream.push_back((uint8_t)chunk);
                    main_stream.insert(main_stream.end(), src + anchor, src + anchor + chunk);
                    anchor += chunk;
                    lit_len -= chunk;
                }

                // B. Extend Match
                size_t curr = ip;
                size_t ref = best_ref;
                std::vector<uint8_t> diff_indices;
                std::vector<uint8_t> diff_values;
                
                while (curr < input_size && (curr - ip) < MAX_MATCH_LEN) {
                    uint8_t s = src[curr];
                    uint8_t r = src[ref];
                    
                    if (s != r) {
                        if (diff_indices.size() >= MAX_DIFFS) break;
                        diff_indices.push_back((uint8_t)(curr - ip));
                        diff_values.push_back(s ^ r);
                    }
                    curr++; ref++;
                }
                size_t match_len = curr - ip;
                
                if (match_len < 4) { ip++; continue; }

                // C. Emit Token
                main_stream.push_back((uint8_t)(0x80 | match_len));
                uint16_t off = (uint16_t)(ip - best_ref);
                main_stream.push_back(off & 0xFF);
                main_stream.push_back(off >> 8);

                main_stream.push_back((uint8_t)diff_indices.size());
                for(auto idx : diff_indices) main_stream.push_back(idx);
                for(auto val : diff_values) delta_stream.push_back(val);

                ip += match_len;
                anchor = ip;
            } else {
                ip++;
            }
        }
    }

    // Finish Literals
    size_t lit_len = input_size - anchor;
    while (lit_len > 0) {
        size_t chunk = std::min(lit_len, (size_t)127);
        if (chunk == 127) main_stream.push_back(0x7F);
        else main_stream.push_back((uint8_t)chunk);
        main_stream.insert(main_stream.end(), src + anchor, src + anchor + chunk);
        anchor += chunk;
        lit_len -= chunk;
    }

    // --- Final Encoding ---
    uint8_t* out_ptr = output.data();
    size_t out_max = output.size();
    size_t current_pos = 16; 

    if (out_max < current_pos) throw std::runtime_error("Buffer too small for header");

    uint64_t total_orig = input_size;
    std::memcpy(out_ptr, &total_orig, 8);

    // 1. Huffman (Main Stream)
    HuffmanCompressor huff;
    size_t h_size = huff.compress(main_stream, output.subspan(current_pos), 0);
    std::memcpy(out_ptr + 8, &h_size, 4);
    current_pos += h_size;

    // 2. Delta Stream (Raw or FSE)
    // We use the MSB of the size field to indicate "Raw Copy"
    if (!delta_stream.empty()) {
        bool use_raw = true;
        size_t f_size = 0;

        // Try FSE only if stream is large enough
        if (delta_stream.size() >= 64) {
            FSECompressor fse;
            // Compress into temp buffer to verify size
            std::vector<uint8_t> temp_buf(fse.get_max_compressed_size(delta_stream.size()));
            f_size = fse.compress(delta_stream, temp_buf, 0);
            
            if (f_size > 0 && f_size < delta_stream.size()) {
                // FSE Successful
                if (current_pos + f_size > out_max) throw std::runtime_error("Buffer overflow");
                std::memcpy(output.data() + current_pos, temp_buf.data(), f_size);
                
                uint32_t flag = (uint32_t)f_size; // MSB 0
                std::memcpy(out_ptr + 12, &flag, 4);
                current_pos += f_size;
                use_raw = false;
            }
        }

        if (use_raw) {
            // Raw Copy
            if (current_pos + delta_stream.size() > out_max) throw std::runtime_error("Buffer overflow");
            std::memcpy(output.data() + current_pos, delta_stream.data(), delta_stream.size());
            
            uint32_t raw_len = (uint32_t)delta_stream.size();
            uint32_t flag = raw_len | 0x80000000; // Set MSB 1
            std::memcpy(out_ptr + 12, &flag, 4);
            current_pos += raw_len;
        }
    } else {
        uint32_t zero = 0;
        std::memcpy(out_ptr + 12, &zero, 4);
    }

    return current_pos;
}

size_t FuzzyLZCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;
    if (input.size() < 16) throw std::runtime_error("Header too small");

    const uint8_t* in_ptr = input.data();
    uint64_t total_orig;
    uint32_t h_size, f_size_flag;

    std::memcpy(&total_orig, in_ptr, 8);
    std::memcpy(&h_size, in_ptr + 8, 4);
    std::memcpy(&f_size_flag, in_ptr + 12, 4);

    if (output.size() < total_orig) throw std::runtime_error("Output buffer too small");

    // Decode Huffman (Main Stream)
    std::vector<uint8_t> main_stream(total_orig + 4096); 
    HuffmanCompressor huff;
    size_t main_len = 0;
    
    if (h_size > 0) {
        main_len = huff.decompress(input.subspan(16, h_size), main_stream);
        main_stream.resize(main_len);
    }

    // Decode Delta Stream
    std::vector<uint8_t> delta_stream; 
    
    bool is_raw_delta = (f_size_flag & 0x80000000) != 0;
    uint32_t f_size = f_size_flag & 0x7FFFFFFF;

    if (f_size > 0) {
        if (is_raw_delta) {
             delta_stream.resize(f_size);
             if (16 + h_size + f_size > input.size()) throw std::runtime_error("Input truncated");
             std::memcpy(delta_stream.data(), input.data() + 16 + h_size, f_size);
        } else {
             delta_stream.resize(total_orig); // Max possible size
             FSECompressor fse;
             size_t d_len = fse.decompress(input.subspan(16 + h_size, f_size), delta_stream);
             delta_stream.resize(d_len);
        }
    }
    
    size_t mp = 0; 
    size_t dp = 0; 
    size_t op = 0; 
    uint8_t* dst = output.data();

    while (op < total_orig && mp < main_stream.size()) {
        uint8_t token = main_stream[mp++];
        
        if ((token & 0x80) == 0) {
            // Literal
            int len = token;
            if (len == 0) continue; 
            if (op + len > total_orig) throw std::runtime_error("Literal overflow");
            if (mp + len > main_stream.size()) throw std::runtime_error("Main stream underflow");
            
            std::memcpy(dst + op, main_stream.data() + mp, len);
            mp += len;
            op += len;
        } 
        else {
            // Match
            int len = token & 0x7F;
            if (len == 0) len = 127; 
            
            if (mp + 3 > main_stream.size()) throw std::runtime_error("Main stream underflow (meta)");
            
            uint16_t off = main_stream[mp++];
            off |= (uint16_t)main_stream[mp++] << 8;
            uint8_t diff_count = main_stream[mp++];
            
            if (op < off) throw std::runtime_error("Invalid offset");
            const uint8_t* ref_ptr = dst + op - off;
            
            for (int i = 0; i < len; ++i) dst[op + i] = ref_ptr[i];

            if (mp + diff_count > main_stream.size()) throw std::runtime_error("Main stream underflow (idx)");
            if (dp + diff_count > delta_stream.size()) throw std::runtime_error("Delta stream underflow");

            for (int i = 0; i < diff_count; ++i) {
                uint8_t idx = main_stream[mp++];
                uint8_t xor_val = delta_stream[dp++];
                if (idx >= len) throw std::runtime_error("Diff index out of bounds");
                dst[op + idx] ^= xor_val;
            }

            op += len;
        }
    }

    return total_orig;
}
