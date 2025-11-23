#pragma once

enum class CompressorType : int {
    NONE = 0,
    HUFFMAN = 1,
    LZ_2B = 2,
    LZ_3B = 3,
    FSE = 4,
    DELTA_FSE = 5,
};
