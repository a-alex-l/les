import os
import time
import subprocess
import csv
import glob
import sys

# --- Configuration ---
# Path to your compiled 'les' executable. 
# Adjust this if your build folder is named differently.
LES_EXECUTABLE = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/app/les"))

# Data directory
DATA_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../data"))

# Output CSV
CSV_FILE = "benchmark_results.csv"

# --- Helper Functions ---

def get_file_size(path):
    try:
        return os.path.getsize(path)
    except FileNotFoundError:
        return 0

def run_command(cmd, output_file=None):
    """
    Runs a shell command and times it.
    If output_file is provided (file object), stdout is redirected there.
    """
    try:
        start_time = time.perf_counter()
        
        if output_file:
            # Run with redirection (for gzip/bzip2 stdout handling)
            subprocess.run(cmd, check=True, stdout=output_file, stderr=subprocess.DEVNULL)
        else:
            # Run normally
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
        end_time = time.perf_counter()
        return end_time - start_time
    except subprocess.CalledProcessError:
        # Command failed
        return None

# --- Benchmark Logic ---

def main():
    # 1. Locate Datasets
    datasets = glob.glob(os.path.join(DATA_DIR, "*.tar"))
    if not datasets:
        print(f"No .tar files found in {DATA_DIR}")
        return

    # 2. Verify LES exists
    if not os.path.exists(LES_EXECUTABLE):
        print(f"Error: Could not find 'les' executable at: {LES_EXECUTABLE}")
        print("Please build the project first.")
        return

    # 3. Define Apps with CUSTOM LEVELS
    apps = {
        "les": {
            "exe": LES_EXECUTABLE,
            "ext": ".les",
            "levels": range(1, 10), # 1 to 9
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", str(lvl), "-i", inp, "-o", out],
            "decompress_cmd": lambda exe, inp, out: [exe, "-i", inp, "-o", out],
            "use_stdout": False
        },
        "gzip": {
            "exe": "gzip",
            "ext": ".gz",
            "levels": range(1, 10, 2), # 1 to 9
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", f"-{lvl}", inp],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-c", inp],
            "use_stdout": True 
        },
        "lz4": {
            "exe": "lz4",
            "ext": ".lz4",
            "levels": range(1, 10, 2), # 1 to 9
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-f", f"-{lvl}", inp, out],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-f", inp, out],
            "use_stdout": False
        },
        "zstd": {
            "exe": "zstd",
            "ext": ".zst",
            # Zstd supports 1-19 standard, 20-22 ultra. 
            # We pick a representative spread to keep benchmark time reasonable.
            "levels": range(1, 18, 2), 
            "compress_cmd": lambda exe, lvl, inp, out: [exe, f"-{lvl}", inp, "-o", out, "-f"],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", inp, "-o", out, "-f"],
            "use_stdout": False
        },
        "bzip2": {
            "exe": "bzip2",
            "ext": ".bz2",
            "levels": range(1, 10, 2), # 1 to 9
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", f"-{lvl}", inp],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-c", inp],
            "use_stdout": True
        }
    }

    results = []

    # Table Header
    print(f"{'Dataset':<20} | {'App':<6} | {'Lv':<2} | {'Ratio':<7} | {'C.Speed':<8} | {'D.Speed':<8} | {'Status'}")
    print("-" * 85)

    for dataset_path in datasets:
        dataset_name = os.path.basename(dataset_path)
        original_size = get_file_size(dataset_path)
        
        if original_size == 0:
            continue

        for app_name, app_config in apps.items():
            # Check if system tool exists
            if app_name != "les" and subprocess.call(["which", app_config["exe"]], stdout=subprocess.DEVNULL) != 0:
                print(f"Skipping {app_name} (not installed)")
                continue

            # Iterate through the CUSTOM levels defined for this app
            for level in app_config["levels"]:
                
                compressed_file = dataset_path + app_config["ext"]
                decompressed_file = dataset_path + ".tmp.decomp"
                
                # Clean up previous runs
                if os.path.exists(compressed_file): os.remove(compressed_file)
                if os.path.exists(decompressed_file): os.remove(decompressed_file)

                # --- 1. Compression ---
                c_cmd = app_config["compress_cmd"](app_config["exe"], level, dataset_path, compressed_file)
                
                c_duration = None
                if app_config["use_stdout"]:
                    with open(compressed_file, "wb") as f_out:
                        c_duration = run_command(c_cmd, output_file=f_out)
                else:
                    c_duration = run_command(c_cmd)

                if c_duration is not None:
                    compressed_size = get_file_size(compressed_file)
                    ratio = compressed_size / original_size
                    c_speed = (original_size / (1024 * 1024)) / c_duration

                    # --- 2. Decompression ---
                    d_cmd = app_config["decompress_cmd"](app_config["exe"], compressed_file, decompressed_file)
                    
                    d_duration = None
                    if app_config["use_stdout"]:
                        with open(decompressed_file, "wb") as f_out:
                            d_duration = run_command(d_cmd, output_file=f_out)
                    else:
                        d_duration = run_command(d_cmd)

                    # --- 3. Verification ---
                    status = "FAIL"
                    d_speed = 0.0
                    
                    if d_duration is not None:
                        d_speed = (original_size / (1024 * 1024)) / d_duration
                        # Verify size matches
                        if get_file_size(decompressed_file) == original_size:
                            status = "OK"
                        else:
                            status = "SizeErr"
                    else:
                        status = "RunErr"

                    # Print result row
                    print(f"{dataset_name[:20]:<20} | {app_name:<6} | {level:<2} | {ratio:.4f}  | {c_speed:6.1f}   | {d_speed:6.1f}   | {status}")

                    # Save result
                    results.append({
                        "Dataset Name": dataset_name,
                        "App": app_name,
                        "Level": level,
                        "Compression Ratio": round(ratio, 5),
                        "Compression Speed": round(c_speed, 2),
                        "Decompression Speed": round(d_speed, 2),
                        "Status": status
                    })

                else:
                    print(f"{dataset_name[:20]:<20} | {app_name:<6} | {level:<2} | FAILED COMPRESSION")

                # Clean up temp files
                if os.path.exists(compressed_file): os.remove(compressed_file)
                if os.path.exists(decompressed_file): os.remove(decompressed_file)

    # 4. Write CSV
    with open(CSV_FILE, 'w', newline='') as csvfile:
        fieldnames = ["Dataset Name", "App", "Level", "Compression Ratio", "Compression Speed", "Decompression Speed", "Status"]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)

        writer.writeheader()
        for row in results:
            writer.writerow(row)

    print(f"\nBenchmark finished. Results saved to {os.path.abspath(CSV_FILE)}")

if __name__ == "__main__":
    main()