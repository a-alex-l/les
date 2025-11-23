# LES Compression

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)
![Standard](https://img.shields.io/badge/c%2B%2B-20-orange)

**LES** is an experimental, single threaded high-performance(not yet) lossless compression utility. 

It is a unique project developed using a **human-in-the-loop AI architecture**:
*   **Tech Lead:** Human Programmer
*   **Core Developer:** AI (Google Gemini 3.0)

The goal is to iteratively compete with industry-standard algorithms (LZ4, Zstd, Gzip) by leveraging AI to write optimized C++20 code, while a human creates the architecture and benchmark harness.

## 🚀 Features

*   **Hybrid Algorithm Selection:** Automatically classifies data chunks to select the optimal compression strategy:
    *   **LZ (Lempel-Ziv):** For structural redundancy and repeating patterns.
    *   **FSE (Finite State Entropy):** A modern entropy coder (tANS) for high-efficiency symbol packing.
    *   **Huffman:** A classic fallback for specific entropy distributions.
*   **Dynamic Chunking:** variable block sizes based on compression levels.
*   **Modular Architecture:** Easy to swap or add new compression backends.
*   **Benchmark Suite:** Includes a Python-based harness to compare against system tools.

## 🛠️ Build Instructions

### Prerequisites
*   C++20 compatible compiler (GCC 10+, Clang 10+, MSVC 2019+)
*   CMake 3.14+

### Building

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
```

Note: The build system automatically fetches dependencies (cxxopts, googletest) via CMake's FetchContent.

## 💻 Usage

The compiled binary is located in build/app/les.

Compression: 
Compress a file (defaults to Level 9):

```./app/les -c -i data.xml -o data.xml.les```

Specify a compression level (1 = Fast, 9 = Best):


```./app/les -c 1 -i data.xml```

Decompression:
Decompression is the default mode (or explicit via flags):

```./app/les -i data.xml.les -o restored.xml```

## 📊 Benchmarks

To benchmark run:

```./bench/bench.py```

## 📜 License

Distributed under the MIT License. See LICENSE for more information.
