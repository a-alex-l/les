import os
import requests
import zipfile
import tarfile
import io

# --- Configuration ---
# Matt Mahoney's mirror is stable and fast
SILESIA_URL = "https://mattmahoney.net/dc/silesia.zip"
DATA_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../data"))

def download_silesia():
    print(f"Downloading Silesia Corpus from {SILESIA_URL}...")
    
    try:
        # Download the file
        response = requests.get(SILESIA_URL, stream=True)
        response.raise_for_status()
        
        print("Download complete. Extracting and converting to TARs...")
        
        # Create data directory
        os.makedirs(DATA_DIR, exist_ok=True)

        # Load zip from memory
        with zipfile.ZipFile(io.BytesIO(response.content)) as z:
            # The zip contains files directly (dickens, mozilla, etc.)
            for filename in z.namelist():
                if filename.endswith('/') or filename.startswith('__'):
                    continue
                
                print(f"  Processing: {filename}")
                
                # Extract raw file to disk temporarily
                z.extract(filename, path=DATA_DIR)
                raw_path = os.path.join(DATA_DIR, filename)
                
                # Create a .tar for this single file
                # This ensures 'les' benchmarks purely on this data type
                tar_name = f"dataset_silesia_{filename}.tar"
                tar_path = os.path.join(DATA_DIR, tar_name)
                
                with tarfile.open(tar_path, "w") as tar:
                    tar.add(raw_path, arcname=filename)
                
                # Cleanup raw file
                os.remove(raw_path)

        print(f"\nSuccess! Datasets prepared in: {DATA_DIR}")
        print("You can now run 'python bench.py'")

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    download_silesia()
