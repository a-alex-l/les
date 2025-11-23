#pragma once
#include <array>
#include <span>
#include <cstdint>

std::array<unsigned int, 256> count_symbols(std::span<const uint8_t> input);

double get_entropy(std::span<const uint8_t> input);
