#include "huffman_compressor.h"
#include <vector>
#include <queue>
#include <map>
#include <stdexcept>
#include <algorithm>

namespace {
    // A node in the Huffman tree
    struct HuffmanNode {
        uint8_t data;
        size_t freq;
        HuffmanNode *left;
        HuffmanNode *right;

        HuffmanNode(uint8_t data, size_t freq) : data(data), freq(freq), left(nullptr), right(nullptr) {}
        ~HuffmanNode() {
            delete left;
            delete right;
        }
    };

    // Comparison for the priority queue
    struct CompareNodes {
        bool operator()(HuffmanNode* l, HuffmanNode* r) {
            return l->freq > r->freq;
        }
    };

    // Recursively generates Huffman codes from the Huffman tree
    void generate_codes(HuffmanNode* root, const std::string& str, std::map<uint8_t, std::string>& huffman_codes) {
        if (!root) {
            return;
        }

        if (!root->left && !root->right) {
            huffman_codes[root->data] = str;
        }

        generate_codes(root->left, str + "0", huffman_codes);
        generate_codes(root->right, str + "1", huffman_codes);
    }
}

size_t HuffmanCompressor::get_max_compressed_size(size_t input_size) const {
    // A robust upper bound must account for:
    // 1. Original size (sizeof(size_t))
    // 2. Frequency map size (sizeof(size_t))
    // 3. The full frequency map (256 * (key + value))
    // 4. The data itself, which in the worst case can expand slightly.
    // 5. A padding byte.
    return input_size + sizeof(size_t) + sizeof(size_t) + (256 * (sizeof(uint8_t) + sizeof(size_t))) + 1;
}

size_t HuffmanCompressor::compress(std::span<const uint8_t> input, std::span<uint8_t> output, int level) {
    if (input.empty()) {
        return 0;
    }

    // The level parameter is not used in this basic Huffman implementation.

    // Calculate frequency of each byte
    std::map<uint8_t, size_t> freq_map;
    for (uint8_t byte : input) {
        freq_map[byte]++;
    }

    // --- FIX START: Handle the edge case of only one unique symbol ---
    if (freq_map.size() == 1) {
        size_t output_index = 0;
        if (output.size() < (2 * sizeof(size_t) + sizeof(uint8_t) + sizeof(size_t))) {
             throw std::runtime_error("Output buffer too small for single-symbol header.");
        }
        // Header: original size, and map size (1)
        *reinterpret_cast<size_t*>(&output[output_index]) = input.size();
        output_index += sizeof(size_t);
        *reinterpret_cast<size_t*>(&output[output_index]) = 1;
        output_index += sizeof(size_t);
        // Write the single symbol and its frequency
        output[output_index++] = freq_map.begin()->first;
        *reinterpret_cast<size_t*>(&output[output_index]) = freq_map.begin()->second;
        output_index += sizeof(size_t);
        // No data body is needed as the frequency implies repetition
        return output_index;
    }
    // --- FIX END ---

    // Build the Huffman tree using a priority queue
    std::priority_queue<HuffmanNode*, std::vector<HuffmanNode*>, CompareNodes> pq;
    for (auto const& [key, val] : freq_map) {
        pq.push(new HuffmanNode(key, val));
    }

    while (pq.size() > 1) {
        HuffmanNode* left = pq.top();
        pq.pop();
        HuffmanNode* right = pq.top();
        pq.pop();

        auto* top = new HuffmanNode('$', left->freq + right->freq);
        top->left = left;
        top->right = right;
        pq.push(top);
    }

    HuffmanNode* root = pq.top();

    // Generate Huffman codes
    std::map<uint8_t, std::string> huffman_codes;
    generate_codes(root, "", huffman_codes);

    delete root;

    // Write header: original size and frequency map
    size_t output_index = 0;
    if (output.size() < sizeof(size_t)) {
        throw std::runtime_error("Output buffer too small for header.");
    }
    *reinterpret_cast<size_t*>(&output[output_index]) = input.size();
    output_index += sizeof(size_t);

    if (output.size() < output_index + sizeof(size_t)) {
        throw std::runtime_error("Output buffer too small for header.");
    }
    *reinterpret_cast<size_t*>(&output[output_index]) = freq_map.size();
    output_index += sizeof(size_t);

    for (auto const& [key, val] : freq_map) {
        if (output.size() < output_index + sizeof(uint8_t) + sizeof(size_t)) {
            throw std::runtime_error("Output buffer too small for frequency table.");
        }
        output[output_index++] = key;
        *reinterpret_cast<size_t*>(&output[output_index]) = val;
        output_index += sizeof(size_t);
    }

    // Write compressed data
    uint8_t current_byte = 0;
    uint8_t bit_count = 0;

    for (uint8_t byte : input) {
        std::string code = huffman_codes[byte];
        for (char bit : code) {
            current_byte = (current_byte << 1) | (bit - '0');
            bit_count++;
            if (bit_count == 8) {
                if (output_index >= output.size()) {
                    throw std::runtime_error("Output buffer too small for compressed data.");
                }
                output[output_index++] = current_byte;
                current_byte = 0;
                bit_count = 0;
            }
        }
    }

    if (bit_count > 0) {
        current_byte <<= (8 - bit_count);
        if (output_index >= output.size()) {
            throw std::runtime_error("Output buffer too small for compressed data.");
        }
        output[output_index++] = current_byte;
    }

    return output_index;
}

size_t HuffmanCompressor::decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if (input.empty()) {
        return 0;
    }

    size_t input_index = 0;

    // Read header
    if (input.size() < input_index + sizeof(size_t)) {
        throw std::runtime_error("Invalid compressed data: unexpected end of header.");
    }
    size_t original_size = *reinterpret_cast<const size_t*>(&input[input_index]);
    input_index += sizeof(size_t);

    if (output.size() < original_size) {
        throw std::runtime_error("Output buffer is too small.");
    }

    if (input.size() < input_index + sizeof(size_t)) {
        throw std::runtime_error("Invalid compressed data: unexpected end of header.");
    }
    size_t freq_map_size = *reinterpret_cast<const size_t*>(&input[input_index]);
    input_index += sizeof(size_t);

    std::map<uint8_t, size_t> freq_map;
    for (size_t i = 0; i < freq_map_size; ++i) {
        if (input.size() < input_index + sizeof(uint8_t) + sizeof(size_t)) {
            throw std::runtime_error("Invalid compressed data: malformed frequency table.");
        }
        uint8_t key = input[input_index++];
        size_t val = *reinterpret_cast<const size_t*>(&input[input_index]);
        input_index += sizeof(size_t);
        freq_map[key] = val;
    }

    // --- FIX START: Handle the edge case of only one unique symbol ---
    if (freq_map_size == 1) {
        if (freq_map.begin()->second != original_size) {
            throw std::runtime_error("Decompression failed: single symbol frequency mismatch.");
        }
        uint8_t symbol = freq_map.begin()->first;
        std::fill(output.begin(), output.begin() + original_size, symbol);
        return original_size;
    }
    // --- FIX END ---

    // Rebuild Huffman tree
    std::priority_queue<HuffmanNode*, std::vector<HuffmanNode*>, CompareNodes> pq;
    for (auto const& [key, val] : freq_map) {
        pq.push(new HuffmanNode(key, val));
    }

    while (pq.size() > 1) {
        HuffmanNode* left = pq.top();
        pq.pop();
        HuffmanNode* right = pq.top();
        pq.pop();

        auto* top = new HuffmanNode('$', left->freq + right->freq);
        top->left = left;
        top->right = right;
        pq.push(top);
    }

    HuffmanNode* root = pq.top();
    HuffmanNode* current_node = root;
    size_t output_index = 0;

    // Decompress data
    for (size_t i = input_index; i < input.size() && output_index < original_size; ++i) {
        uint8_t byte = input[i];
        for (int j = 7; j >= 0; --j) {
            uint8_t bit = (byte >> j) & 1;
            if (bit == 0) {
                current_node = current_node->left;
            } else {
                current_node = current_node->right;
            }

            if (!current_node->left && !current_node->right) {
                if (output_index >= output.size()) {
                    delete root;
                    throw std::runtime_error("Output buffer too small during decompression.");
                }
                output[output_index++] = current_node->data;
                current_node = root;
                if (output_index == original_size) {
                    break;
                }
            }
        }
    }

    delete root;

    if (output_index != original_size) {
        throw std::runtime_error("Decompression failed: output size does not match original size.");
    }

    return output_index;
}
