#include "fuzzy_lz_compressor.h"
#include "fse_compressor.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <memory> // for std::align
#include <stdexcept>
#include <vector>

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
constexpr int THRESH_D = 2;
constexpr int THRESH_X = 1;
constexpr size_t MAX_SHIFT = 65535;
enum MatchType : uint8_t {
  TYPE_LITERAL = 0,
  TYPE_EXACT = 1,
  TYPE_DIFF = 2,
  TYPE_XOR = 3
};
constexpr uint32_t HASH_MUL = 0x1e35a7bd;
constexpr int HASH_LOG = 16;
constexpr int HASH_SIZE = 1 << HASH_LOG;

// Alignment and padding safety
constexpr size_t ALIGNMENT = 64;
constexpr size_t PADDING = 64;

inline uint32_t hash_func(uint32_t val) {
  return (val * HASH_MUL) >> (32 - HASH_LOG);
}
inline uint32_t read32_le(const uint8_t *ptr) {
  return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}
inline void write32_le(uint8_t *ptr, uint32_t val) {
  ptr[0] = val & 0xFF;
  ptr[1] = (val >> 8) & 0xFF;
  ptr[2] = (val >> 16) & 0xFF;
  ptr[3] = (val >> 24) & 0xFF;
}
inline uint64_t read64_le(const uint8_t *ptr) {
  uint64_t val;
  std::memcpy(&val, ptr, 8);
  return val;
}

// Robust allocator that handles alignment and advances the span safely
template <typename T>
std::span<T> alloc_span(std::span<uint8_t> &memory, size_t count) {
  if (memory.empty() || count == 0)
    return {};

  void *ptr = memory.data();
  size_t space = memory.size();

  // Align the pointer
  void *aligned_ptr = std::align(alignof(T), count * sizeof(T), ptr, space);

  if (!aligned_ptr) {
    throw std::bad_alloc();
  }

  // Calculate consumed bytes (padding + data)
  size_t padding_bytes = static_cast<uint8_t *>(aligned_ptr) - memory.data();
  size_t data_bytes = count * sizeof(T);
  size_t total_consumed = padding_bytes + data_bytes;

  if (memory.size() < total_consumed) {
    throw std::bad_alloc();
  }

  // Advance memory span
  memory = memory.subspan(total_consumed);

  return std::span<T>(static_cast<T *>(aligned_ptr), count);
}

template <typename T> struct ScratchVector {
  std::span<T> data;
  size_t count = 0;
  ScratchVector() = default;

  // Uses the robust alloc_span internally
  ScratchVector(std::span<uint8_t> &mem, size_t capacity) {
    data = alloc_span<T>(mem, capacity);
  }

  void push_back(T val) {
    if (count < data.size()) {
      data[count++] = val;
    } else {
      // Safety fallback to prevent segfaults if estimates were slightly off,
      // though logic should prevent this.
      data[data.size() - 1] = val;
    }
  }
  std::span<T> filled() { return data.subspan(0, count); }
};

} // namespace

size_t FuzzyLZCompressor::get_max_compressed_size(size_t input_size) const {
  // 6 streams overhead + FSE headers + safety margin
  return input_size + (6 * 1050) + 8192;
}

size_t
FuzzyLZCompressor::get_compression_scratch_size(size_t input_size) const {
  // 1. Stream Buffers (Component arrays)
  // We need enough space to store the decomposed streams before FSE
  // compression. In the worst case (all literals), s_type, s_lit, etc. scale
  // with input_size.
  size_t stream_buffers =
      (input_size + PADDING) * 3 +    // lit, fuzzy, type
      (input_size / 3 + PADDING) * 3; // len, shift_lo, shift_hi

  // 2. Hash Tables (only needed during LZ pass)
  size_t hash_tables =
      (HASH_SIZE * sizeof(uint32_t)) + (HASH_SIZE * sizeof(uint8_t));

  // 3. FSE Scratch
  // FSE needs scratch proportional to the size of the buffer being compressed.
  // The largest possible buffer to compress is ~input_size (s_lit).
  FSECompressor fse;
  size_t fse_scratch = fse.get_compression_scratch_size(input_size + PADDING);

  // Layout Strategy:
  // [Streams .....] [Hash Tables]
  //                 [FSE Scratch (Reusing Hash Table memory + extra)]
  //
  // So we need: Size(Streams) + Max(Size(Hash), Size(FSE_Scratch))
  // plus alignment overhead for each allocation (approx 8 allocs * 64 bytes)

  size_t ephemeral_space = std::max(hash_tables, fse_scratch);

  return stream_buffers + ephemeral_space + (16 * ALIGNMENT);
}

size_t
FuzzyLZCompressor::get_decompression_scratch_size(size_t output_size) const {
  // 1. Stream Buffers (reconstructed)
  size_t stream_buffers =
      (output_size + PADDING) * 3 + (output_size / 3 + PADDING) * 3;

  // 2. FSE Decompression Scratch
  // We decompress one stream at a time. Largest stream is ~output_size.
  FSECompressor fse;
  size_t fse_scratch =
      fse.get_decompression_scratch_size(output_size + PADDING);

  return stream_buffers + fse_scratch + (16 * ALIGNMENT);
}

size_t FuzzyLZCompressor::compress(std::span<const uint8_t> input,
                                   std::span<uint8_t> output,
                                   std::span<uint8_t> scratch, int level) {
  const uint8_t *data = input.data();
  size_t len = input.size();
  size_t i = 0;

  // Keep a copy of the scratch iterator to handle the "ephemeral" memory
  // section
  std::span<uint8_t> current_scratch = scratch;

  // 1. Allocate Stream Buffers (Persistent throughout function)
  auto s_type = ScratchVector<uint8_t>(current_scratch, len + 64);
  auto s_len = ScratchVector<uint8_t>(current_scratch, len / 3 + 64);
  auto s_shift_lo = ScratchVector<uint8_t>(current_scratch, len / 3 + 64);
  auto s_shift_hi = ScratchVector<uint8_t>(current_scratch, len / 3 + 64);
  auto s_lit = ScratchVector<uint8_t>(current_scratch, len + 64);
  auto s_fuzzy = ScratchVector<uint8_t>(current_scratch, len + 64);

  // Mark the point where streams end. Memory after this is reusable between
  // phases.
  std::span<uint8_t> ephemeral_scratch = current_scratch;

  // 2. Allocate Hash Tables (Phase 1 only)
  auto hash_table = ScratchVector<uint32_t>(current_scratch, HASH_SIZE);
  auto hash_valid = ScratchVector<uint8_t>(current_scratch, HASH_SIZE);

  // Initialize Hash
  std::fill(hash_table.data.begin(), hash_table.data.end(), 0);
  std::fill(hash_valid.data.begin(), hash_valid.data.end(), 0);

  // --- LZ Pass ---
  while (i < len) {
    if (i + 4 > len) {
      s_type.push_back(TYPE_LITERAL);
      s_lit.push_back(data[i]);
      i++;
      continue;
    }
    uint32_t sequence = read32_le(data + i);
    uint32_t h = hash_func(sequence);
    size_t ref = hash_table.data[h];
    bool valid = hash_valid.data[h];

    // Update Hash
    hash_table.data[h] = static_cast<uint32_t>(i);
    hash_valid.data[h] = 1;

    bool match_found = false;
    if (valid && (i > ref) && ((i - ref) <= MAX_SHIFT)) {
      if (read32_le(data + ref) == sequence)
        match_found = true;
    }
    if (!match_found) {
      s_type.push_back(TYPE_LITERAL);
      s_lit.push_back(data[i]);
      i++;
      continue;
    }
    size_t l_anc = 0;
    while ((i + l_anc < len) && (data[i + l_anc] == data[ref + l_anc]))
      l_anc++;

    // Fuzzy matching logic
    size_t l_diff = 0;
    while (i + l_anc + l_diff < len) {
      int delta = std::abs(static_cast<int>(data[i + l_anc + l_diff]) -
                           static_cast<int>(data[ref + l_anc + l_diff]));
      if (delta > THRESH_D)
        break;
      l_diff++;
    }
    size_t l_xor = 0;
    while (i + l_anc + l_xor < len) {
      uint8_t val = data[i + l_anc + l_xor] ^ data[ref + l_anc + l_xor];
      int bits = count_set_bits(val);
      if (bits > THRESH_X)
        break;
      l_xor++;
    }

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
      size_t store_len = total_len - 3;
      while (store_len >= 255) {
        s_len.push_back(255);
        store_len -= 255;
      }
      s_len.push_back(static_cast<uint8_t>(store_len));

      size_t shift = i - ref;
      s_shift_lo.push_back(shift & 0xFF);
      s_shift_hi.push_back((shift >> 8) & 0xFF);

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

  // --- FSE Pass ---
  FSECompressor fse;
  uint8_t *op = output.data();
  const uint8_t *oend = output.data() + output.size();

  // IMPORTANT: Reset the scratch space to overwrite Hash Tables.
  // We use `ephemeral_scratch`, which starts right after the stream vectors.
  // This provides maximum space for FSE.

  auto compress_stream = [&](std::span<uint8_t> src) {
    if (op + 4 > oend)
      throw std::runtime_error("Buffer full");

    // Check empty
    if (src.empty()) {
      write32_le(op, 0);
      op += 4;
      return;
    }

    uint8_t *hdr = op;
    op += 4;

    // We must pass a copy of the scratch span because FSE modifies the
    // pointer/state
    std::span<uint8_t> fse_scratch_buffer = ephemeral_scratch;

    // Safety check inside FSE call usually, but good to check here
    if (op >= oend)
      throw std::runtime_error("Buffer full");

    size_t sz = fse.compress(src, std::span<uint8_t>(op, oend - op),
                             fse_scratch_buffer, level);

    write32_le(hdr, static_cast<uint32_t>(sz));
    op += sz;
  };

  compress_stream(s_type.filled());
  compress_stream(s_len.filled());
  compress_stream(s_shift_lo.filled());
  compress_stream(s_shift_hi.filled());
  compress_stream(s_lit.filled());
  compress_stream(s_fuzzy.filled());

  return static_cast<size_t>(op - output.data());
}

size_t FuzzyLZCompressor::decompress(std::span<const uint8_t> input,
                                     std::span<uint8_t> output,
                                     std::span<uint8_t> scratch) {
  FSECompressor fse;
  const uint8_t *ip = input.data();
  const uint8_t *iend = input.data() + input.size();
  size_t osz = output.size();

  // Maintain scratch pointer
  std::span<uint8_t> current_scratch = scratch;

  // Allocate all stream buffers first (must exist simultaneously)
  auto s_type = ScratchVector<uint8_t>(current_scratch, osz + 64);
  auto s_len = ScratchVector<uint8_t>(current_scratch, osz / 3 + 64);
  auto s_shift_lo = ScratchVector<uint8_t>(current_scratch, osz / 3 + 64);
  auto s_shift_hi = ScratchVector<uint8_t>(current_scratch, osz / 3 + 64);
  auto s_lit = ScratchVector<uint8_t>(current_scratch, osz + 64);
  auto s_fuzzy = ScratchVector<uint8_t>(current_scratch, osz + 64);

  // The remaining memory can be used for FSE decompression scratch
  std::span<uint8_t> fse_scratch_buffer = current_scratch;

  auto decompress_stream = [&](ScratchVector<uint8_t> &dest) {
    if (ip + 4 > iend)
      throw std::runtime_error("Missing frame header");
    uint32_t csize = read32_le(ip);
    ip += 4;

    if (csize == 0) {
      dest.count = 0;
      return;
    }

    if (ip + csize > iend)
      throw std::runtime_error("Stream truncated");

    // FSE Header is at least 8 bytes
    if (csize < 8)
      throw std::runtime_error("FSE blob too small");

    uint64_t usize = read64_le(ip);

    // Ensure ScratchVector has enough allocated space
    if (usize > dest.data.size())
      throw std::runtime_error("Scratch buffer too small for stream");

    // Reuse the remaining scratch for FSE
    std::span<uint8_t> local_fse_scratch = fse_scratch_buffer;

    size_t w = fse.decompress(std::span<const uint8_t>(ip, csize), dest.data,
                              local_fse_scratch);

    if (w != usize)
      throw std::runtime_error("Size mismatch");

    dest.count = usize;
    ip += csize;
  };

  decompress_stream(s_type);
  decompress_stream(s_len);
  decompress_stream(s_shift_lo);
  decompress_stream(s_shift_hi);
  decompress_stream(s_lit);
  decompress_stream(s_fuzzy);

  // Reconstruction Loop
  size_t out_pos = 0;
  size_t out_cap = output.size();
  uint8_t *out_buf = output.data();
  size_t p_len = 0, p_slo = 0, p_shi = 0, p_lit = 0, p_fuz = 0;

  for (size_t k = 0; k < s_type.count; ++k) {
    if (out_pos >= out_cap)
      break;
    uint8_t type = s_type.data[k];

    if (type == TYPE_LITERAL) {
      if (p_lit >= s_lit.count)
        throw std::runtime_error("Stream underrun: Lit");
      out_buf[out_pos++] = s_lit.data[p_lit++];
    } else {
      size_t match_len = 0;
      while (p_len < s_len.count) {
        uint8_t v = s_len.data[p_len++];
        match_len += v;
        if (v < 255)
          break;
      }
      match_len += 3;

      if (p_slo >= s_shift_lo.count || p_shi >= s_shift_hi.count)
        throw std::runtime_error("Stream underrun: Shift");

      size_t shift = s_shift_lo.data[p_slo++] |
                     (static_cast<size_t>(s_shift_hi.data[p_shi++]) << 8);

      if (shift == 0 || shift > out_pos)
        throw std::runtime_error("Invalid shift");

      size_t ref_pos = out_pos - shift;
      if (out_pos + match_len > out_cap)
        throw std::runtime_error("Output overflow");

      if (type == TYPE_EXACT) {
        for (size_t j = 0; j < match_len; ++j)
          out_buf[out_pos++] = out_buf[ref_pos + j];
      } else if (type == TYPE_DIFF) {
        if (p_fuz + match_len > s_fuzzy.count)
          throw std::runtime_error("Stream underrun: Fuzzy");
        for (size_t j = 0; j < match_len; ++j)
          out_buf[out_pos++] = out_buf[ref_pos + j] + s_fuzzy.data[p_fuz++];
      } else if (type == TYPE_XOR) {
        if (p_fuz + match_len > s_fuzzy.count)
          throw std::runtime_error("Stream underrun: Fuzzy");
        for (size_t j = 0; j < match_len; ++j)
          out_buf[out_pos++] = out_buf[ref_pos + j] ^ s_fuzzy.data[p_fuz++];
      }
    }
  }
  return out_pos;
}
