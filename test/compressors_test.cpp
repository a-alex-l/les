#include <gtest/gtest.h>
#include "huffman_compressor.h"
#include "fse_compressor.h"
#include "lz_compressor.h"
#include "fuzzy_lz_compressor.h"
#include <string>
#include <vector>
#include <numeric>
#include <random>
#include <algorithm>
#include <iostream> // Added for cout
#include <iomanip>  // Added for debug formatting

// Helper function to convert a string to a vector of uint8_t
std::vector<uint8_t> to_u8_vec(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Helper to escape strings for copy-pasting (e.g., turns " into \")
std::string escape_for_code(const std::string& s) {
    std::stringstream ss;
    ss << "\"";
    for (char c : s) {
        if (c == '\\') ss << "\\\\";
        else if (c == '"') ss << "\\\"";
        else ss << c;
    }
    ss << "\"";
    return ss.str();
}

template <typename T>
class CompressorTest : public ::testing::Test {};

TYPED_TEST_SUITE_P(CompressorTest);

TYPED_TEST_P(CompressorTest, RoundTrip) {
    auto generateTestInputs = []() {
        std::vector<std::string> inputs = {
            "",
            "a",
            "hello world",
            "ababababababababababab",
            "The quick brown fox jumps over the lazy dog.",
            std::string(1000, 'a'),
            std::string(1024, 's'),
            std::string(4096, 'F'),
            std::string(1000000, 'a'),
            std::string(1000000, 'a') + std::string(100000, 'b'),
            "abcdefghijklmnopqrstuvwxyz",
            "AAAAAAAAABAAAAAAAAABAAAAAAAAAB"
        };
        inputs.push_back(std::string("Start\0End", 9));

        std::string window_killer = "ABCDEF";
        for (int i = 0; i < 2000; ++i)
            window_killer += "1234567890"; 
        window_killer += "ABCDEF";
        inputs.push_back(window_killer);

        const size_t binary_data_size = 2 * 1024;
        std::string binary_data;
        binary_data.resize(binary_data_size);
        std::mt19937 gen(12345);
        std::uniform_int_distribution<> dist_byte(0, 255);
        std::generate(binary_data.begin(), binary_data.end(), [&]() { return static_cast<char>(dist_byte(gen)); });
        inputs.push_back(binary_data);

        std::random_device rd;
        std::mt19937 gen_visible(rd());
        std::uniform_int_distribution<> dist_visible(32, 126);
        std::uniform_int_distribution<> dist_len(1, 5000);

        for(int i = 0; i < 100; ++i) {
            int len = dist_len(gen_visible);
            std::string s;
            s.reserve(len);
            for(int j = 0; j < len; ++j) {
                s.push_back(static_cast<char>(dist_visible(gen_visible)));
            }
            inputs.push_back(s);
        }

        return inputs;
    };

    for (const auto& original_str : generateTestInputs()) {

        bool is_printable = std::all_of(original_str.begin(), original_str.end(), [](unsigned char c){ 
            return c >= 32 && c <= 126; 
        });

        if (is_printable && original_str.length() < 300) {
           // std::cout << "[ TESTING ] " << escape_for_code(original_str) << std::endl;
        }

        for (int level = -12; level < 10; ++level) {

            std::vector<uint8_t> original_data = to_u8_vec(original_str);

            TypeParam compressor;

            SCOPED_TRACE("Input String: " + (original_str.length() > 100 && !is_printable ? "[Binary Data]" : escape_for_code(original_str)));
            SCOPED_TRACE("Compression Level: " + std::to_string(level));

            std::vector<uint8_t> compressed_data(compressor.get_max_compressed_size(original_data.size()));
            std::vector<uint8_t> decompressed_data(original_data.size());

            size_t compressed_size = 0;

            try {
                compressed_size = compressor.compress(original_data, compressed_data, level);
            } catch (const std::exception& e) {
                FAIL() << "Compression threw exception: " << e.what();
            }

            compressed_data.resize(compressed_size);

            size_t decompressed_size = 0;
            try {
                decompressed_size = compressor.decompress(compressed_data, decompressed_data);
            } catch (const std::exception& e) {
                 FAIL() << "Decompression threw exception: " << e.what();
            }

            ASSERT_EQ(decompressed_size, original_data.size()) << "Size mismatch.";
            ASSERT_EQ(original_data, decompressed_data) << "Content mismatch.";
        }
    }
}

REGISTER_TYPED_TEST_SUITE_P(CompressorTest, RoundTrip);
using CompressorTypes = ::testing::Types<
    HuffmanCompressor,
    FSECompressor,
    LZCompressor<LZMode::V2B>,
    LZCompressor<LZMode::V3B>,
    FuzzyLZCompressor
>;
INSTANTIATE_TYPED_TEST_SUITE_P(AllCompressors, CompressorTest, CompressorTypes);