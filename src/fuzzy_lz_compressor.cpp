#include "fuzzy_lz_compressor.h"
#include "fse_compressor.h"

#include <vector>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <bit> 

// Fallback for popcount
#if __cplusplus < 202002L
    #ifdef _MSC_VER
        #include <intrin.h>
        inline int count_set_bits(uint8_t v) { return __popcnt16(v); }
    #else
        inline int count_set_bits(uint8_t v) { return __builtin_popcount(v); }
    #endif
#else
    inline int count_set_bits(uint8_t v) { return std::popcount(v); }
#endif

namespace {
    // FuzzyLZ Constants
    constexpr int THRESH_D = 2;
    constexpr int THRESH_X = 1;
    constexpr size_t MAX_SHIFT = 65535;

    enum MatchType : uint8_t {
        TYPE_LITERAL = 0,
        TYPE_EXACT   = 1,
        TYPE_DIFF    = 2,
        TYPE_XOR     = 3
    };

    // Rolling Hash
    constexpr uint32_t HASH_MUL = 0x1e35a7bd;
    constexpr int HASH_LOG = 16;
    constexpr int HASH_SIZE = 1 << HASH_LOG;

    inline uint32_t hash_func(uint32_t val) {
        return (val * HASH_MUL) >> (32 - HASH_LOG);
    }

    // Byte Reading Helpers
    inline uint32_t read32_le(const uint8_t* ptr) {
        return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    }

    inline void write32_le(uint8_t* ptr, uint32_t val) {
        ptr[0] = val & 0xFF;
        ptr[1] = (val >> 8) & 0xFF;
        ptr[2] = (val >> 16) & 0xFF;
        ptr[3] = (val >> 24) & 0xFF;
    }

    inline uint64_t read64_le(const uint8_t* ptr) {
        uint64_t val;
        std::memcpy(&val, ptr, 8);
        return val;
    }
}

size_t FuzzyLZCompressor::get_max_compressed_size(size_t input_size) const {
    FSECompressor fse;
    // Overhead: 6 streams * (~1050 bytes FSE header) + Framing + Margin
    return input_size + (6 * 1050) + 4096;
}

size_t FuzzyLZCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    const uint8_t* data = input.data();
    size_t len = input.size();
    size_t i = 0;

    // Stream Buffers
    std::vector<uint8_t> s_type;     s_type.reserve(len / 4);
    std::vector<uint8_t> s_len;      s_len.reserve(len / 8);
    std::vector<uint8_t> s_shift_lo; s_shift_lo.reserve(len / 8);
    std::vector<uint8_t> s_shift_hi; s_shift_hi.reserve(len / 8);
    std::vector<uint8_t> s_lit;      s_lit.reserve(len / 2);
    std::vector<uint8_t> s_fuzzy;    s_fuzzy.reserve(len / 8);

    std::vector<uint32_t> hash_table(HASH_SIZE, 0);
    std::vector<bool> hash_valid(HASH_SIZE, false);

    while (i < len) {
        if (i + 4 > len) {
            s_type.push_back(TYPE_LITERAL);
            s_lit.push_back(data[i]);
            i++;
            continue;
        }

        uint32_t sequence = read32_le(data + i);
        uint32_t h = hash_func(sequence);
        
        size_t ref = hash_table[h];
        bool valid = hash_valid[h];
        
        hash_table[h] = static_cast<uint32_t>(i);
        hash_valid[h] = true;

        bool match_found = false;
        if (valid && (i > ref) && ((i - ref) <= MAX_SHIFT)) {
            if (read32_le(data + ref) == sequence) {
                match_found = true;
            }
        }

        if (!match_found) {
            s_type.push_back(TYPE_LITERAL);
            s_lit.push_back(data[i]);
            i++;
            continue;
        }

        // Anchor
        size_t l_anc = 0;
        while ((i + l_anc < len) && (data[i + l_anc] == data[ref + l_anc])) {
            l_anc++;
        }

        // Diff
        size_t l_diff = 0;
        while (i + l_anc + l_diff < len) {
            int delta = std::abs(static_cast<int>(data[i + l_anc + l_diff]) - static_cast<int>(data[ref + l_anc + l_diff]));
            if (delta > THRESH_D) break;
            l_diff++;
        }

        // Xor
        size_t l_xor = 0;
        while (i + l_anc + l_xor < len) {
            uint8_t val = data[i + l_anc + l_xor] ^ data[ref + l_anc + l_xor];
            int bits = count_set_bits(val);
            if (bits > THRESH_X) break;
            l_xor++;
        }

        // Decision
        size_t total_len = l_anc;
        MatchType mode = TYPE_EXACT;

        if (l_diff > 0 || l_xor > 0) {
            if (l_diff >= l_xor) {
                mode = TYPE_DIFF;
                total_len += l_diff;
            } else {
                mode = TYPE_XOR;
                total_len += l_xor;
            }
        }

        if (total_len < 3) {
            s_type.push_back(TYPE_LITERAL);
            s_lit.push_back(data[i]);
            i++;
        } else {
            s_type.push_back(mode);

            // Length encoding
            size_t store_len = total_len - 3;
            while (store_len >= 255) {
                s_len.push_back(255);
                store_len -= 255;
            }
            s_len.push_back(static_cast<uint8_t>(store_len));

            // Shift encoding
            size_t shift = i - ref;
            s_shift_lo.push_back(shift & 0xFF);
            s_shift_hi.push_back((shift >> 8) & 0xFF);

            // Fuzzy data for WHOLE match
            if (mode == TYPE_DIFF) {
                for (size_t k = 0; k < total_len; ++k)
                    s_fuzzy.push_back(static_cast<uint8_t>(data[i + k] - data[ref + k]));
            } else if (mode == TYPE_XOR) {
                for (size_t k = 0; k < total_len; ++k)
                    s_fuzzy.push_back(data[i + k] ^ data[ref + k]);
            }
            i += total_len;
        }
    }

    FSECompressor fse;
    uint8_t* op = output.data();
    const uint8_t* oend = output.data() + output.size();

    auto compress_stream = [&](const std::vector<uint8_t>& src) {
        if (op + 4 > oend) throw std::runtime_error("Buffer full");
        uint8_t* hdr = op; op += 4;
        
        size_t sz = 0;
        if (!src.empty()) {
            if (op >= oend) throw std::runtime_error("Buffer full");
            sz = fse.compress(src, std::span<uint8_t>(op, oend - op), level);
        }
        write32_le(hdr, static_cast<uint32_t>(sz));
        op += sz;
    };

    compress_stream(s_type);
    compress_stream(s_len);
    compress_stream(s_shift_lo);
    compress_stream(s_shift_hi);
    compress_stream(s_lit);
    compress_stream(s_fuzzy);

    return static_cast<size_t>(op - output.data());
}

size_t FuzzyLZCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    FSECompressor fse;
    const uint8_t* ip = input.data();
    const uint8_t* iend = input.data() + input.size();

    auto decompress_stream = [&](std::vector<uint8_t>& dest) {
        if (ip + 4 > iend) throw std::runtime_error("Missing frame header");
        uint32_t csize = read32_le(ip);
        ip += 4;
        if (csize == 0) { dest.clear(); return; }
        
        if (ip + csize > iend) throw std::runtime_error("Stream truncated");
        
        // Peek Uncompressed Size from FSE Header
        if (csize < 8) throw std::runtime_error("FSE blob too small");
        uint64_t usize = read64_le(ip);
        
        if (usize > 1024ULL * 1024 * 1024 * 2) throw std::runtime_error("Stream too large"); // Sanity limit 2GB
        
        dest.resize(static_cast<size_t>(usize));
        
        size_t w = fse.decompress(std::span<const uint8_t>(ip, csize), std::span<uint8_t>(dest));
        if (w != usize) throw std::runtime_error("Size mismatch");
        ip += csize;
    };

    std::vector<uint8_t> s_type, s_len, s_shift_lo, s_shift_hi, s_lit, s_fuzzy;
    
    decompress_stream(s_type);
    decompress_stream(s_len);
    decompress_stream(s_shift_lo);
    decompress_stream(s_shift_hi);
    decompress_stream(s_lit);
    decompress_stream(s_fuzzy);

    size_t out_pos = 0;
    size_t out_cap = output.size();
    uint8_t* out_buf = output.data();

    size_t p_len = 0, p_slo = 0, p_shi = 0, p_lit = 0, p_fuz = 0;

    for (uint8_t type : s_type) {
        if (out_pos >= out_cap) break; 

        if (type == TYPE_LITERAL) {
            if (p_lit >= s_lit.size()) throw std::runtime_error("Stream underrun: Lit");
            out_buf[out_pos++] = s_lit[p_lit++];
        } else {
            // Reconstruct Match
            size_t match_len = 0;
            while (p_len < s_len.size()) {
                uint8_t v = s_len[p_len++];
                match_len += v;
                if (v < 255) break;
            }
            match_len += 3;

            if (p_slo >= s_shift_lo.size() || p_shi >= s_shift_hi.size()) 
                throw std::runtime_error("Stream underrun: Shift");
            
            size_t shift = s_shift_lo[p_slo++] | (static_cast<size_t>(s_shift_hi[p_shi++]) << 8);
            if (shift == 0 || shift > out_pos) throw std::runtime_error("Invalid shift");
            
            size_t ref_pos = out_pos - shift;
            if (out_pos + match_len > out_cap) throw std::runtime_error("Output overflow");

            if (type == TYPE_EXACT) {
                // Handle Overlap for LZ
                for (size_t k = 0; k < match_len; ++k)
                    out_buf[out_pos++] = out_buf[ref_pos + k];
            } else if (type == TYPE_DIFF) {
                if (p_fuz + match_len > s_fuzzy.size()) throw std::runtime_error("Stream underrun: Fuzzy");
                for (size_t k = 0; k < match_len; ++k)
                    out_buf[out_pos++] = out_buf[ref_pos + k] + s_fuzzy[p_fuz++];
            } else if (type == TYPE_XOR) {
                if (p_fuz + match_len > s_fuzzy.size()) throw std::runtime_error("Stream underrun: Fuzzy");
                for (size_t k = 0; k < match_len; ++k)
                    out_buf[out_pos++] = out_buf[ref_pos + k] ^ s_fuzzy[p_fuz++];
            }
        }
    }

    return out_pos;
}
