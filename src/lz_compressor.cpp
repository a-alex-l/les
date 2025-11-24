#include "lz_compressor.h"
#include "fse_compressor.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
constexpr size_t MATCH_LEN_THRESHOLD = 255;

inline size_t get_hash(const uint8_t *p, size_t hash_bits) {
  uint32_t val;
  std::memcpy(&val, p, 4);
  return (val * 0x9E3779B1) >> (32 - hash_bits);
}

// --- Theoretical Maximum Calculators ---

static constexpr size_t align4(size_t size) { return (size + 3) & ~3; }

static constexpr size_t max_controls_size(size_t input_size) {
  return (input_size + 7) / 8 + 64;
}

static constexpr size_t max_literals_size(size_t input_size) {
  return input_size + 64;
}

static constexpr size_t max_lengths_size(size_t input_size) {
  return (input_size / 3) + 64;
}

static constexpr size_t max_offsets_size(size_t input_size) {
  return ((input_size / 3) * 2) + 64;
}

// Bump allocator helper
template <typename T> struct ScratchVector {
  std::span<T> data;
  size_t count = 0;

  ScratchVector() = default;
  ScratchVector(std::span<uint8_t> &mem, size_t capacity) {
    size_t bytes = capacity * sizeof(T);
    size_t padding = (4 - (bytes % 4)) % 4;
    size_t total_bytes = bytes + padding;

    if (mem.size() < total_bytes)
      throw std::bad_alloc();

    data = std::span<T>(reinterpret_cast<T *>(mem.data()), capacity);
    mem = mem.subspan(total_bytes);
  }

  void push_back(T val) {
    if (count < data.size()) {
      data[count++] = val;
    } else {
      // Logic error fallback: overwrite last byte to prevent heap corruption
      if (!data.empty())
        data[data.size() - 1] = val;
    }
  }

  std::span<T> filled() { return data.subspan(0, count); }
  bool empty() const { return count == 0; }
  void clear() { count = 0; }
};

struct StreamSplitter {
  ScratchVector<uint8_t> controls;
  ScratchVector<uint8_t> literals;
  ScratchVector<uint8_t> lengths;
  ScratchVector<uint8_t> offsets;

  uint8_t current_control = 0;
  int token_count = 0;

  StreamSplitter(std::span<uint8_t> &scratch, size_t input_size) {
    controls = ScratchVector<uint8_t>(scratch, max_controls_size(input_size));
    literals = ScratchVector<uint8_t>(scratch, max_literals_size(input_size));
    lengths = ScratchVector<uint8_t>(scratch, max_lengths_size(input_size));
    offsets = ScratchVector<uint8_t>(scratch, max_offsets_size(input_size));
  }

  void add_literal(uint8_t lit) {
    literals.push_back(lit);
    next_token();
  }

  void add_match(size_t len, size_t dist, size_t min_match_len) {
    current_control |= (1 << token_count);
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
    offsets.push_back(static_cast<uint8_t>(dist & 0xFF));
    offsets.push_back(static_cast<uint8_t>((dist >> 8) & 0xFF));
    next_token();
  }

  void next_token() {
    token_count++;
    if (token_count == 8)
      flush_control();
  }

  void flush_control() {
    if (token_count > 0) {
      controls.push_back(current_control);
      current_control = 0;
      token_count = 0;
    }
  }
  void finish() { flush_control(); }
};

} // namespace

template <LZMode Mode>
size_t LZCompressor<Mode>::get_max_compressed_size(size_t input_size) const {
  // Theoretical max + headers + large safety padding
  return input_size + (input_size / 8) + 8192;
}

template <LZMode Mode>
size_t
LZCompressor<Mode>::get_compression_scratch_size(size_t input_size) const {
  constexpr size_t HASH_SIZE = 1 << 16;
  constexpr size_t WINDOW_SIZE = 65536;

  // 1. Calculate space needed for LZ vectors
  size_t vec_overhead = 0;
  vec_overhead += align4(max_controls_size(input_size));
  vec_overhead += align4(max_literals_size(input_size));
  vec_overhead += align4(max_lengths_size(input_size));
  vec_overhead += align4(max_offsets_size(input_size));

  // 2. Calculate space for the "Reuse Area" (Memory shared between LZ Tables
  // and FSE Scratch) We must reserve enough space for whichever is larger to
  // avoid FSE overwriting the vectors later.
  size_t lz_tables_size = (HASH_SIZE * 4) + (WINDOW_SIZE * 4);

  FSECompressor fse;
  // FSE needs scratch proportional to the data it compresses (worst case:
  // literals buffer)
  size_t fse_scratch_needed =
      fse.get_compression_scratch_size(max_literals_size(input_size));

  size_t reserved_reuse_area =
      align4(std::max(lz_tables_size, fse_scratch_needed));

  return reserved_reuse_area + vec_overhead + 8192;
}

template <LZMode Mode>
size_t
LZCompressor<Mode>::get_decompression_scratch_size(size_t output_size) const {
  size_t sz_ctrl = align4(max_controls_size(output_size));
  size_t sz_lit = align4(max_literals_size(output_size));
  size_t sz_len = align4(max_lengths_size(output_size));
  size_t sz_off = align4(max_offsets_size(output_size));

  size_t vec_overhead = sz_ctrl + sz_lit + sz_len + sz_off;

  FSECompressor fse;
  return vec_overhead + fse.get_decompression_scratch_size(output_size) + 8192;
}

template <LZMode Mode>
size_t LZCompressor<Mode>::compress(std::span<const uint8_t> input,
                                    std::span<uint8_t> output,
                                    std::span<uint8_t> scratch, int level) {
  if (input.empty())
    return 0;

  constexpr size_t WINDOW_SIZE = 65536;
  constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
  constexpr size_t MIN_MATCH_LEN = 3;
  constexpr size_t HASH_BITS = 16;
  constexpr size_t HASH_SIZE = 1 << HASH_BITS;
  constexpr size_t NICE_MATCH_LEN = 256;

  // Keep a pointer to the start of scratch for the FSE reuse later
  uint8_t *const scratch_base = scratch.data();
  const size_t scratch_total_size = scratch.size();

  // --- 1. Memory Layout Setup ---
  // Recalculate the reserved area size to match get_compression_scratch_size
  // exactly
  FSECompressor fse;
  size_t lz_tables_size = (HASH_SIZE * 4) + (WINDOW_SIZE * 4);
  size_t fse_scratch_needed =
      fse.get_compression_scratch_size(max_literals_size(input.size()));
  size_t reserved_reuse_area =
      align4(std::max(lz_tables_size, fse_scratch_needed));

  if (scratch.size() < reserved_reuse_area)
    throw std::runtime_error("Scratch buffer too small for reuse area");

  // Setup LZ Tables in the first part of the reserved area
  auto head_span =
      ScratchVector<int32_t>(scratch, HASH_SIZE); // advances 'scratch'
  auto prev_span =
      ScratchVector<int32_t>(scratch, WINDOW_SIZE); // advances 'scratch'
  int32_t *head = head_span.data.data();
  int32_t *prev = prev_span.data.data();

  std::fill(head_span.data.begin(), head_span.data.end(), -1);
  std::fill(prev_span.data.begin(), prev_span.data.end(), -1);

  // SKIP PADDING!
  // We must skip any remaining bytes in the reserved area so that 'streams'
  // starts strictly AFTER 'reserved_reuse_area'.
  size_t used_so_far = scratch.data() - scratch_base;
  if (reserved_reuse_area > used_so_far) {
    size_t padding = reserved_reuse_area - used_so_far;
    if (scratch.size() < padding)
      throw std::bad_alloc();
    scratch = scratch.subspan(padding);
  }

  // --- 2. Stream Splitting ---
  StreamSplitter streams(scratch, input.size());

  // --- 3. LZ Compression Loop ---
  const uint8_t *ip = input.data();
  const uint8_t *const ip_start = ip;
  const uint8_t *const ip_end = ip + input.size();
  const uint8_t *const ip_limit = ip_end - 5;

  int eff_level = (level < 1) ? 1 : level;
  uint32_t max_chain = (1u << eff_level);
  if (max_chain > 4096)
    max_chain = 4096;

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
        if (dist >= WINDOW_SIZE || dist == 0)
          break;
        const uint8_t *match_ptr = ip_start + match_idx;
        if (ip[best_len] == match_ptr[best_len] && ip[0] == match_ptr[0]) {
          size_t len = 0;
          while (ip + len < ip_end && ip[len] == match_ptr[len])
            len++;
          if (len > best_len) {
            best_len = len;
            best_dist = dist;
            if (best_len >= NICE_MATCH_LEN)
              break;
          }
        }
        match_idx = prev[match_idx & WINDOW_MASK];
        chain_len++;
      }
    }

    if (best_len >= MIN_MATCH_LEN) {
      streams.add_match(best_len, best_dist, MIN_MATCH_LEN);
      const uint8_t *next_ip = ip + best_len;
      ip++;
      while (ip < next_ip) {
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

  // --- 4. Output Writing ---
  uint8_t *op = output.data();
  uint8_t *const op_end = op + output.size();

  if (op + 8 > op_end)
    throw std::runtime_error("Output too small for header");
  uint64_t total_size = static_cast<uint64_t>(input.size());
  std::memcpy(op, &total_size, 8);
  op += 8;

  if (op + 16 > op_end)
    throw std::runtime_error("Output too small for stream headers");
  uint8_t *size_ptr = op;
  op += 16;

  // --- 5. FSE Compression (Reusing Scratch) ---
  // We reuse the ENTIRE reserved area at the start.
  // Since we enforced that 'streams' starts after 'reserved_reuse_area',
  // FSE can use the whole reserved area without touching 'streams'.
  std::span<uint8_t> fse_reuse_scratch(scratch_base, reserved_reuse_area);

  auto compress_stream = [&](std::span<uint8_t> src, uint8_t *&dest, int idx) {
    if (src.empty()) {
      std::memset(size_ptr + (idx * 4), 0, 4);
      return;
    }

    if (dest >= op_end)
      throw std::runtime_error("Output buffer exhausted");

    size_t remaining = op_end - dest;
    if (remaining < 64)
      throw std::runtime_error("Output buffer exhausted before FSE stream");

    size_t comp_sz = fse.compress(src, std::span<uint8_t>(dest, remaining),
                                  fse_reuse_scratch, level);

    if (dest + comp_sz > op_end)
      throw std::runtime_error("FSE wrote past end of buffer");

    uint32_t sz_u32 = static_cast<uint32_t>(comp_sz);
    std::memcpy(size_ptr + (idx * 4), &sz_u32, 4);
    dest += comp_sz;
  };

  compress_stream(streams.controls.filled(), op, 0);
  compress_stream(streams.literals.filled(), op, 1);
  compress_stream(streams.lengths.filled(), op, 2);
  compress_stream(streams.offsets.filled(), op, 3);

  return op - output.data();
}

template <LZMode Mode>
size_t LZCompressor<Mode>::decompress(std::span<const uint8_t> input,
                                      std::span<uint8_t> output,
                                      std::span<uint8_t> scratch) {
  if (input.empty())
    return 0;

  const uint8_t *ip = input.data();
  const uint8_t *ip_end = ip + input.size();

  if (input.size() < 24)
    throw std::runtime_error("Input too small");

  uint64_t original_size;
  std::memcpy(&original_size, ip, 8);
  ip += 8;

  if (output.size() < original_size)
    throw std::runtime_error("Output buffer too small");

  uint32_t sz_ctrl, sz_lit, sz_len, sz_off;
  std::memcpy(&sz_ctrl, ip + 0, 4);
  std::memcpy(&sz_lit, ip + 4, 4);
  std::memcpy(&sz_len, ip + 8, 4);
  std::memcpy(&sz_off, ip + 12, 4);
  ip += 16;

  FSECompressor fse;

  // Decompression scratch management is simpler: we allocate buffers first,
  // then whatever remains is given to FSE.
  auto buf_ctrl =
      ScratchVector<uint8_t>(scratch, max_controls_size(original_size));
  auto buf_lit =
      ScratchVector<uint8_t>(scratch, max_literals_size(original_size));
  auto buf_len =
      ScratchVector<uint8_t>(scratch, max_lengths_size(original_size));
  auto buf_off =
      ScratchVector<uint8_t>(scratch, max_offsets_size(original_size));

  auto decompress_stream = [&](size_t comp_sz, ScratchVector<uint8_t> &dest) {
    if (comp_sz == 0)
      return;
    if (ip + comp_sz > ip_end)
      throw std::runtime_error("Truncated FSE stream");

    // Pass the remaining scratch to FSE
    size_t written = fse.decompress(std::span<const uint8_t>(ip, comp_sz),
                                    dest.data, scratch);

    if (written > dest.data.size())
      throw std::runtime_error("FSE decompressed more than expected");

    dest.count = written;
    ip += comp_sz;
  };

  decompress_stream(sz_ctrl, buf_ctrl);
  decompress_stream(sz_lit, buf_lit);
  decompress_stream(sz_len, buf_len);
  decompress_stream(sz_off, buf_off);

  uint8_t *op = output.data();
  size_t lit_idx = 0;
  size_t len_idx = 0;
  size_t off_idx = 0;
  size_t bytes_decoded = 0;
  constexpr size_t MIN_MATCH_LEN = 3;

  for (size_t c = 0; c < buf_ctrl.count; ++c) {
    uint8_t control = buf_ctrl.data[c];
    for (int i = 0; i < 8 && bytes_decoded < original_size; ++i) {
      if ((control >> i) & 1) {
        // Match
        if (len_idx >= buf_len.count)
          throw std::runtime_error("Len buffer underflow");

        size_t len_code = buf_len.data[len_idx++];
        size_t match_len = len_code + MIN_MATCH_LEN;

        if (len_code == 255) {
          while (len_idx < buf_len.count) {
            uint8_t ext = buf_len.data[len_idx++];
            match_len += ext;
            if (ext != 255)
              break;
          }
        }

        if (off_idx + 1 >= buf_off.count)
          throw std::runtime_error("Off buffer underflow");

        size_t dist = static_cast<size_t>(buf_off.data[off_idx]) |
                      (static_cast<size_t>(buf_off.data[off_idx + 1]) << 8);
        off_idx += 2;

        if (dist == 0 || dist > bytes_decoded)
          throw std::runtime_error("Invalid distance");

        if (bytes_decoded + match_len > original_size)
          throw std::runtime_error("Decoded data exceeds original size");

        const uint8_t *src = op - dist;
        for (size_t k = 0; k < match_len; ++k)
          *op++ = *src++;
        bytes_decoded += match_len;
      } else {
        // Literal
        if (lit_idx >= buf_lit.count)
          throw std::runtime_error("Lit buffer underflow");

        if (bytes_decoded >= original_size)
          throw std::runtime_error("Decoded data exceeds original size");

        *op++ = buf_lit.data[lit_idx++];
        bytes_decoded++;
      }
    }
  }
  return bytes_decoded;
}

template class LZCompressor<LZMode::V2B>;
template class LZCompressor<LZMode::V3B>;
