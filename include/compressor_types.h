#pragma once

enum class CompressorType : int {
  NONE = 0,
  FSE = 1,
  LZ_2B = 2,
  LZ_3B = 3,
  DELTA_FSE = 4,
  FUZZY_LZ_3B = 5,
};
