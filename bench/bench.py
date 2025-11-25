import os
import time
import subprocess
import csv
import glob
import sys
import platform

# --- NEW IMPORT ---
try:
    import psutil
except ImportError:
    print("Error: 'psutil' module not found. Please run: pip install psutil")
    sys.exit(1)

# --- Configuration ---
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# CHANGED: Build directory is now inside 'bench/build'
BUILD_DIR = os.path.join(ROOT_DIR, "bench", "build")

DATA_DIR = os.path.join(ROOT_DIR, "data")
CSV_FILE = "benchmark_results.csv"

# Expected executable location
if platform.system() == "Windows":
    LES_PATHS = [
        os.path.join(BUILD_DIR, "app", "Release", "les.exe"),
        os.path.join(BUILD_DIR, "app", "les.exe")
    ]
else:
    LES_PATHS = [
        os.path.join(BUILD_DIR, "app", "les")
    ]

# --- Helper Functions ---

def build_project():
    """
    Compiles the project in Release mode. 
    Forces a clean build to ensure we aren't benchmarking an old Debug binary.
    """
    print(f"--- Building Project in Release Mode ---")
    print(f"Build Directory: {BUILD_DIR}")
    
    try:
        # 1. Configure
        config_cmd = ["cmake", "-S", ROOT_DIR, "-B", BUILD_DIR, "-DCMAKE_BUILD_TYPE=Release"]
        subprocess.check_call(config_cmd, stdout=subprocess.DEVNULL)

        # 2. Build (With --clean-first)
        # --clean-first ensures we delete old object files before compiling
        build_cmd = ["cmake", "--build", BUILD_DIR, "--config", "Release", "--clean-first", "-j"]
        
        print(f"Compiling (Clean Release Build)...")
        subprocess.check_call(build_cmd, stdout=subprocess.DEVNULL)
        print("Build Successful.\n")

    except subprocess.CalledProcessError as e:
        print(f"\nError: Build failed with exit code {e.returncode}")
        sys.exit(1)
    except FileNotFoundError:
        print("\nError: 'cmake' not found. Please install CMake.")
        sys.exit(1)

def find_les_executable():
    for path in LES_PATHS:
        if os.path.exists(path):
            return path
    return None

def get_file_size(path):
    try:
        return os.path.getsize(path)
    except FileNotFoundError:
        return 0

def run_command(cmd, output_file=None):
    try:
        # High precision timer
        start_time = time.perf_counter()
        
        if output_file:
            subprocess.run(cmd, check=True, stdout=output_file, stderr=subprocess.DEVNULL)
        else:
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            
        end_time = time.perf_counter()
        return end_time - start_time
    except subprocess.CalledProcessError:
        return None

# --- Core Pinning ---
def pin_to_best_core():
    print("--- CPU Affinity Setup ---")
    print("Analyzing CPU usage (sampling for 1s)...")
    
    # Get usage per core
    cpu_usage = psutil.cpu_percent(interval=1.0, percpu=True)
    
    # Find index of lowest usage, try to avoid Core 0 if possible
    best_core = 0
    min_usage = 100.0
    
    for i, usage in enumerate(cpu_usage):
        if i == 0 and len(cpu_usage) > 1:
            continue
        if usage < min_usage:
            min_usage = usage
            best_core = i
            
    print(f"Core Usage: {cpu_usage}")
    print(f"Selected Core #{best_core} (Load: {min_usage}%)")
    
    # Pin the current process (and all future children) to this core
    proc = psutil.Process()
    proc.cpu_affinity([best_core])
    
    print(f"Process pinned to Core #{best_core}\n")

# --- Benchmark Logic ---

def main():
    # 1. Compile (Uses ALL cores for speed)
    build_project()

    # 2. Pin to specific core (Now subsequent commands run on single core)
    pin_to_best_core()

    # 3. Locate Executable
    les_exe = find_les_executable()
    if not les_exe:
        print(f"Error: Could not find 'les' executable.")
        print(f"Checked paths: {LES_PATHS}")
        return
    else:
        print(f"Using executable: {les_exe}\n")

    # 4. Locate Datasets
    datasets = glob.glob(os.path.join(DATA_DIR, "*.tar"))
    if not datasets:
        print(f"No .tar files found in {DATA_DIR}")
        return

    # 5. Define Apps
    apps = {
        "les": {
            "exe": les_exe,
            "ext": ".les",
            "levels": range(1, 10),
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", str(lvl), "-i", inp, "-o", out],
            "decompress_cmd": lambda exe, inp, out: [exe, "-i", inp, "-o", out],
            "use_stdout": False
        },
        "gzip": {
            "exe": "gzip",
            "ext": ".gz",
            "levels": range(1, 10, 2),
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", f"-{lvl}", inp],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-c", inp],
            "use_stdout": True 
        },
        "lz4": {
            "exe": "lz4",
            "ext": ".lz4",
            "levels": [1, 2] + list(range(3, 10, 2)),
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-f", f"-{lvl}", inp, out],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-f", inp, out],
            "use_stdout": False
        },
        "zstd": {
            "exe": "zstd",
            "ext": ".zst",
            "levels": range(1, 18, 2), 
            "compress_cmd": lambda exe, lvl, inp, out: [exe, f"-{lvl}", inp, "-o", out, "-f"],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", inp, "-o", out, "-f"],
            "use_stdout": False
        },
        "bzip2": {
            "exe": "bzip2",
            "ext": ".bz2",
            "levels": range(1, 10, 2),
            "compress_cmd": lambda exe, lvl, inp, out: [exe, "-c", f"-{lvl}", inp],
            "decompress_cmd": lambda exe, inp, out: [exe, "-d", "-c", inp],
            "use_stdout": True
        }
    }

    results = []

    print(f"{'Dataset':<20} | {'App':<6} | {'Lv':<2} | {'Ratio':<7} | {'C.Speed':<8} | {'D.Speed':<8} | {'Status'}")
    print("-" * 85)

    for dataset_path in datasets:
        dataset_name = os.path.basename(dataset_path)
        original_size = get_file_size(dataset_path)
        
        if original_size == 0: continue

        for app_name, app_config in apps.items():
            if app_name != "les":
                # Check for existence of system tools
                check_cmd = ["where"] if platform.system() == "Windows" else ["which"]
                if subprocess.call(check_cmd + [app_config["exe"]], stdout=subprocess.DEVNULL) != 0:
                    continue

            for level in app_config["levels"]:
                compressed_file = dataset_path + app_config["ext"]
                decompressed_file = dataset_path + ".tmp.decomp"
                
                if os.path.exists(compressed_file): os.remove(compressed_file)
                if os.path.exists(decompressed_file): os.remove(decompressed_file)

                # Compression
                c_cmd = app_config["compress_cmd"](app_config["exe"], level, dataset_path, compressed_file)
                c_duration = None
                
                if app_config["use_stdout"]:
                    with open(compressed_file, "wb") as f_out:
                        c_duration = run_command(c_cmd, output_file=f_out)
                else:
                    c_duration = run_command(c_cmd)

                if c_duration is not None and c_duration > 0:
                    compressed_size = get_file_size(compressed_file)
                    ratio = compressed_size / original_size
                    c_speed = (original_size / (1024 * 1024)) / c_duration

                    # Decompression
                    d_cmd = app_config["decompress_cmd"](app_config["exe"], compressed_file, decompressed_file)
                    d_duration = None
                    
                    if app_config["use_stdout"]:
                        with open(decompressed_file, "wb") as f_out:
                            d_duration = run_command(d_cmd, output_file=f_out)
                    else:
                        d_duration = run_command(d_cmd)

                    status = "FAIL"
                    d_speed = 0.0
                    
                    if d_duration is not None and d_duration > 0:
                        d_speed = (original_size / (1024 * 1024)) / d_duration
                        if get_file_size(decompressed_file) == original_size:
                            status = "OK"
                        else:
                            status = "SizeErr"
                    else:
                        status = "RunErr"

                    print(f"{dataset_name[:20]:<20} | {app_name:<6} | {level:<2} | {ratio:.4f}  | {c_speed:6.1f}   | {d_speed:6.1f}   | {status}")

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
                    print(f"{dataset_name[:20]:<20} | {app_name:<6} | {level:<2} | FAILED")

                if os.path.exists(compressed_file): os.remove(compressed_file)
                if os.path.exists(decompressed_file): os.remove(decompressed_file)

    with open(CSV_FILE, 'w', newline='') as csvfile:
        fieldnames = ["Dataset Name", "App", "Level", "Compression Ratio", "Compression Speed", "Decompression Speed", "Status"]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        for row in results:
            writer.writerow(row)

    print(f"\nBenchmark finished. Results saved to {os.path.abspath(CSV_FILE)}")

if __name__ == "__main__":
    main()
