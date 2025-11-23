#include <iostream>
#include <string>
#include <vector>
#include "chunker.h" 
#include "cxxopts.hpp"

int main(int argc, char* argv[]) {
    cxxopts::Options options("les", "A fast, parallel compression utility.\nDefault action is decompression.");

    options.add_options()
        // Implicit value allows "les -c" to mean Level 9
        ("c,compress", "Enable compression mode with optional level (1-9)", cxxopts::value<int>()->implicit_value("9"))
        ("i,input", "Input file", cxxopts::value<std::string>())
        ("o,output", "Output file (optional)", cxxopts::value<std::string>())
        ("h,help", "Print usage information")
        // Catch-all for "dangling" arguments (like the '5' in '-c 5')
        ("positional", "Positional arguments", cxxopts::value<std::vector<std::string>>());

    // Tell cxxopts to put unknown non-flag arguments into "positional"
    options.parse_positional({"positional"});

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
            if (compress_mode) {
                output_file = input_file + ".les";
            } else { 
                const std::string extension = ".les";
                if (input_file.length() > extension.length() && 
                    input_file.substr(input_file.length() - extension.length()) == extension) {
                    output_file = input_file.substr(0, input_file.length() - extension.length());
                } else {
                    std::cerr << "Error: Cannot determine output filename. Input does not end with '.les'. Use -o." << std::endl;
                    return 1;
                }
            }
        }
        
        if (input_file == output_file) {
            std::cerr << "Error: Input and output filenames are the same ('" << input_file << "'). Aborting." << std::endl;
            return 1;
        }
        
        Chunker parallelizer;

        if (compress_mode) {
            int level = result["compress"].as<int>();

            // --- TRICK: Check for "dangling" level argument ---
            // If the user typed "-c 5", cxxopts sets level=9 (implicit) and puts "5" in positional.
            // We check if we have a positional number 1-9 and override the level.
            if (result.count("positional")) {
                const auto& pos_args = result["positional"].as<std::vector<std::string>>();
                for (const auto& arg : pos_args) {
                    try {
                        size_t pos = 0;
                        int val = std::stoi(arg, &pos);
                        // If it's a pure number between 1-9
                        if (pos == arg.length() && val >= 1 && val <= 9) {
                            level = val;
                            break; // Use the first valid number we find as the level
                        }
                    } catch (...) {
                        // Not a number, ignore (could be a filename if user messed up args)
                    }
                }
            }
            // --------------------------------------------------

            if (level < 1 || level > 9) {
                std::cerr << "Error: Compression level must be between 1 and 9." << std::endl;
                return 1;
            }
            std::cout << "Compressing '" << input_file << "' to '" << output_file 
                      << "' with Level " << level << "..." << std::endl;
            
            parallelizer.compress_file(input_file, output_file, level);
            std::cout << "Compression finished successfully." << std::endl;
        } else {
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