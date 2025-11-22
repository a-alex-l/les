#include <iostream>
#include <string>
#include <vector>
#include "parallelizer.h" // Your library
#include "cxxopts.hpp"    // The command-line parser

int main(int argc, char* argv[]) {
    cxxopts::Options options("les", "A fast, parallel compression utility.\nDefault action is decompression.");

    options.add_options()
        ("c,compress", "Enable compression mode with optional level (1-9)", cxxopts::value<int>()->implicit_value("9"))
        ("i,input", "Input file", cxxopts::value<std::string>())
        ("o,output", "Output file (optional)", cxxopts::value<std::string>())
        ("h,help", "Print usage information");

    try {
        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        if (!result.count("input")) {
            std::cerr << "Error: Input file is required. Use -i <filename>." << std::endl;
            std::cerr << options.help() << std::endl;
            return 1;
        }

        const bool compress_mode = result.count("compress") > 0;
        const std::string input_file = result["input"].as<std::string>();
        std::string output_file;

        // --- Determine Output Filename ---
        if (result.count("output")) {
            output_file = result["output"].as<std::string>();
        } else {
            // Generate output filename if not provided
            if (compress_mode) {
                output_file = input_file + ".les";
            } else { // Decompression mode
                const std::string extension = ".les";
                // Check if the input file ends with .les
                if (input_file.length() > extension.length() && 
                    input_file.substr(input_file.length() - extension.length()) == extension) {
                    output_file = input_file.substr(0, input_file.length() - extension.length());
                } else {
                    std::cerr << "Error: Cannot automatically determine output filename for decompression." << std::endl;
                    std::cerr << "Input file '" << input_file << "' does not end with '.les'. Please specify the output with -o." << std::endl;
                    return 1;
                }
            }
        }
        
        // Safety check: prevent overwriting the source file
        if (input_file == output_file) {
            std::cerr << "Error: Input and output filenames are the same ('" << input_file << "'). Aborting." << std::endl;
            return 1;
        }
        
        Parallelizer parallelizer;

        if (compress_mode) {
            int level = result["compress"].as<int>();
            if (level < 1 || level > 9) {
                std::cerr << "Error: Compression level must be between 1 and 9." << std::endl;
                return 1;
            }
            std::cout << "Compressing '" << input_file << "' to '" << output_file 
                      << "' with level " << level << std::endl;
            parallelizer.compress_file(input_file, output_file, level);
            std::cout << "Compression finished successfully." << std::endl;
        } else { // Decompression mode (default)
            std::cout << "Decompressing '" << input_file << "' to '" << output_file << "'..." << std::endl;
            parallelizer.decompress_file(input_file, output_file);
            std::cout << "Decompression finished successfully." << std::endl;
        }

    } catch (const cxxopts::exceptions::exception& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
