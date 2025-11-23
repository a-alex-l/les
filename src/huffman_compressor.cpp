#include "huffman_compressor.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <array>
#include <numeric>
#include <span>

namespace {

// ============================================================
// Constants
// ============================================================
constexpr uint ILP = 4;                 // Instruction Level Parallelism
constexpr int MAX_CODE_BITS = 15;       // Hard limit for Deflate-style compatibility
constexpr int TABLE_BITS = 12;          // Main lookup table size (2^12 = 4096 entries)
constexpr int TABLE_SIZE = 1 << TABLE_BITS;

// ============================================================
// 1. SIMD-Friendly Counting (Interleaved)
// ============================================================
struct alignas(64) AlignedHistogram {
    uint data[256];
};

std::array<uint, 256> count_symbols(std::span<const uint8_t> input)
{
    std::array<AlignedHistogram, ILP> streams;
    // Explicit zero init
    for(auto& s : streams) std::fill(std::begin(s.data), std::end(s.data), 0);

    const uint8_t* ptr = input.data();
    size_t len = input.size();

    // Align to 8-byte boundary
    while (len > 0 && (reinterpret_cast<uintptr_t>(ptr) & 7)) {
        streams[0].data[*ptr]++;
        ptr++;
        len--;
    }

    // Unrolled batch processing
    auto run_batch = [&](auto seq) { // 'seq' holds the index_sequence
        [ptr, &streams]<size_t... Is>(std::index_sequence<Is...>) __attribute__((always_inline)) {
            ( (streams[Is].data[ptr[Is]]++), ... );
        }(seq);
    };

    while (len >= ILP) {
        run_batch(std::make_index_sequence<ILP>{});
        ptr += ILP;
        len -= ILP;
    }

    while (len > 0) {
        streams[0].data[*ptr]++;
        ptr++;
        len--;
    }

    std::array<uint, 256> ans = {};
    for (size_t s = 0; s < 256; ++s) {
        uint sum = 0;
        for (size_t i = 0; i < ILP; ++i) sum += streams[i].data[s];
        ans[s] = sum;
    }
    return ans;
}

// ============================================================
// 2. Two-Queue Algorithm (Stack-based, Linear Time)
// ============================================================
std::array<uint8_t, 256> find_lengths_15(const std::array<uint, 256>& counts)
{
    std::array<uint64_t, 512> freqs; 
    std::array<uint16_t, 512> parents; 
    std::array<uint16_t, 256> leaf_queue;
    int leaf_count = 0;

    for (size_t i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            freqs[i] = counts[i];
            leaf_queue[leaf_count++] = (uint16_t)i;
        }
    }

    if (leaf_count == 0) return {}; 
    if (leaf_count == 1) {
        std::array<uint8_t, 256> ans = {};
        ans[leaf_queue[0]] = 1;
        return ans;
    }

    // Sort leaves by frequency
    std::sort(leaf_queue.begin(), leaf_queue.begin() + leaf_count, 
        [&](uint16_t a, uint16_t b) { return freqs[a] < freqs[b]; }
    );

    int head1 = 0, head2 = 0;
    int internal_count = 0;
    int next_node = 256;

    auto pop_min = [&]() -> uint16_t {
        if (head1 >= leaf_count) return (uint16_t)(256 + head2++);
        if (head2 >= internal_count) return leaf_queue[head1++];
        if (freqs[leaf_queue[head1]] < freqs[256 + head2]) return leaf_queue[head1++];
        return (uint16_t)(256 + head2++);
    };

    // Build Tree
    for (int i = 0; i < leaf_count - 1; ++i) {
        uint16_t c1 = pop_min();
        uint16_t c2 = pop_min();
        uint16_t p = (uint16_t)next_node++;
        freqs[p] = freqs[c1] + freqs[c2];
        parents[c1] = p; parents[c2] = p;
        internal_count++;
    }

    std::array<uint8_t, 256> lengths = {};
    uint16_t root = (uint16_t)(next_node - 1);

    // Calc Depths & Clamp
    for (int i = 0; i < leaf_count; ++i) {
        uint16_t node = leaf_queue[i];
        int depth = 0;
        while (node != root) {
            node = parents[node];
            depth++;
        }
        
        // LIMIT TO 15 BITS (Fit into 4-bit header nibble)
        if (depth > MAX_CODE_BITS) depth = MAX_CODE_BITS; 
        
        lengths[leaf_queue[i]] = (uint8_t)depth;
    }
    return lengths;
}

// ============================================================
// 3. Canonical Codes (Bit-Reversed)
// ============================================================
inline uint16_t fast_reverse_bits(uint16_t val, uint8_t bits) {
    val = (val & 0x5555) << 1 | (val & 0xAAAA) >> 1;
    val = (val & 0x3333) << 2 | (val & 0xCCCC) >> 2;
    val = (val & 0x0F0F) << 4 | (val & 0xF0F0) >> 4;
    val = (val & 0x00FF) << 8 | (val & 0xFF00) >> 8;
    return val >> (16 - bits);
}

std::array<uint16_t, 256> generate_codes_reversed(const std::array<uint8_t, 256>& lengths)
{
    std::array<uint16_t, MAX_CODE_BITS + 2> bl_count = {};
    for (uint8_t len : lengths) bl_count[len]++;

    std::array<uint16_t, MAX_CODE_BITS + 2> next_code = {};
    uint16_t code = 0;
    bl_count[0] = 0;
    
    for (int bits = 1; bits <= MAX_CODE_BITS; ++bits) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    std::array<uint16_t, 256> codes = {};
    for (size_t i = 0; i < 256; ++i) {
        uint8_t len = lengths[i];
        if (len != 0) {
            uint16_t raw = next_code[len]++;
            codes[i] = fast_reverse_bits(raw, len);
        }
    }
    return codes;
}

} // anonymous namespace

// ============================================================
// Implementation
// ============================================================

size_t HuffmanCompressor::get_max_compressed_size(size_t input_size) const {
    // Size(8) + PackedTable(128) + Data + Padding(64)
    return sizeof(size_t) + 128 + input_size + 64; 
}

size_t HuffmanCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int) 
{
    if (input.empty()) return 0;
    if (output.size() < get_max_compressed_size(input.size())) {
        throw std::runtime_error("Output buffer too small");
    }

    // 1. Analyze
    auto counts = count_symbols(input);
    auto lengths = find_lengths_15(counts);
    auto codes = generate_codes_reversed(lengths);

    size_t out_pos = 0;
    
    // 2. Header: Original Size
    size_t original_len = input.size();
    std::memcpy(&output[out_pos], &original_len, sizeof(size_t));
    out_pos += sizeof(size_t);

    // 3. Header: Packed Lengths (4 bits per symbol = 128 bytes)
    for (int i = 0; i < 128; ++i) {
        uint8_t l1 = lengths[2 * i];
        uint8_t l2 = lengths[2 * i + 1];
        // Odd in high nibble, Even in low nibble
        output[out_pos++] = (uint8_t)((l2 << 4) | (l1 & 0x0F));
    }

    // 4. Fast Encoding Loop
    uint64_t buffer = 0;
    int bit_count = 0;
    
    const uint8_t* in_ptr = input.data();
    const uint8_t* const in_end = input.data() + input.size();
    uint8_t* out_ptr = output.data() + out_pos;
    
    while (in_ptr < in_end) {
        uint8_t symbol = *in_ptr++;
        
        // Raw Array Lookup
        buffer |= (uint64_t)codes[symbol] << bit_count;
        bit_count += lengths[symbol];

        // Flush 32 bits
        if (bit_count >= 32) {
            // Portable Unaligned Write
            out_ptr[0] = (uint8_t)buffer;
            out_ptr[1] = (uint8_t)(buffer >> 8);
            out_ptr[2] = (uint8_t)(buffer >> 16);
            out_ptr[3] = (uint8_t)(buffer >> 24);
            out_ptr += 4;
            
            buffer >>= 32;
            bit_count -= 32;
        }
    }

    // Flush remainder
    while (bit_count > 0) {
        *out_ptr++ = (uint8_t)buffer;
        buffer >>= 8;
        bit_count -= 8;
    }

    return out_ptr - output.data();
}

size_t HuffmanCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) return 0;

    size_t in_idx = 0;
    
    // 1. Read Size
    if (input.size() < sizeof(size_t) + 128) throw std::runtime_error("Header too short");
    size_t original_size;
    std::memcpy(&original_size, &input[in_idx], sizeof(size_t));
    in_idx += sizeof(size_t);

    if (output.size() < original_size) throw std::runtime_error("Output buffer too small");

    // 2. Read Packed Lengths
    std::array<uint8_t, 256> lengths;
    const uint8_t* packed = &input[in_idx];
    for (int i = 0; i < 128; ++i) {
        uint8_t val = packed[i];
        lengths[2 * i]     = val & 0x0F;
        lengths[2 * i + 1] = (val >> 4) & 0x0F;
    }
    in_idx += 128;

    // 3. Build Decoding Table (Fast 12-bit LUT)
    struct Entry {
        uint16_t symbol; // Storing symbol in u16 for alignment
        uint8_t len;
    };
    
    // 4096 entries (fits in L1/L2 cache)
    static_assert(sizeof(Entry) * TABLE_SIZE <= 16384, "Table too big for stack");
    Entry table[TABLE_SIZE]; 
    std::memset(table, 0, sizeof(table)); // len=0 indicates invalid/empty

    struct LongCode {
        uint16_t code;
        uint8_t len;
        uint8_t symbol;
    };
    std::vector<LongCode> slow_codes;

    uint32_t bl_count[MAX_CODE_BITS + 1] = {0};
    uint32_t next_code[MAX_CODE_BITS + 1] = {0};

    for (int i = 0; i < 256; ++i) if (lengths[i]) bl_count[lengths[i]]++;

    uint32_t code = 0;
    for (int bits = 1; bits <= MAX_CODE_BITS; ++bits) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // Fill Table
    for (int c = 0; c < 256; ++c) {
        int len = lengths[c];
        if (len == 0) continue;

        uint32_t raw = next_code[len]++;
        // Decoder reads LSB first, so we reverse the code to match the stream
        uint32_t rev = fast_reverse_bits((uint16_t)raw, (uint8_t)len);

        if (len <= TABLE_BITS) {
            // Fast Path: Fill all redundant entries
            int step = 1 << len;
            for (int idx = rev; idx < TABLE_SIZE; idx += step) {
                table[idx].symbol = (uint16_t)c;
                table[idx].len = (uint8_t)len;
            }
        } else {
            // Slow Path (Length 13, 14, 15)
            slow_codes.push_back({ (uint16_t)rev, (uint8_t)len, (uint8_t)c });
        }
    }

    // 4. Raw Decoding Loop
    const uint8_t* src = input.data() + in_idx;
    const uint8_t* src_end = input.data() + input.size();
    uint8_t* dst = output.data();
    uint8_t* dst_end = output.data() + original_size;

    uint64_t bit_buffer = 0;
    int bit_count = 0;

    while (dst < dst_end) {
        // Refill Buffer (Safe load up to 64 bits)
        // We ensure we can read at least a few bytes ahead.
        // If near end, be careful (simplified here for speed)
        while (bit_count <= 56 && src < src_end) {
            bit_buffer |= (uint64_t)(*src++) << bit_count;
            bit_count += 8;
        }

        // Peek Table
        uint16_t key = bit_buffer & (TABLE_SIZE - 1);
        Entry e = table[key];

        if (e.len != 0) {
            // Fast Path
            *dst++ = (uint8_t)e.symbol;
            bit_buffer >>= e.len;
            bit_count -= e.len;
        } else {
            // Slow Path (Linear scan is fine for rare codes > 12 bits)
            bool found = false;
            // Scan through long codes (very few exist)
            for (const auto& lc : slow_codes) {
                uint64_t mask = (1ULL << lc.len) - 1;
                if ((bit_buffer & mask) == lc.code) {
                    *dst++ = lc.symbol;
                    bit_buffer >>= lc.len;
                    bit_count -= lc.len;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("Corrupt Stream");
        }
    }

    return original_size;
}