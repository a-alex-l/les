import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import os
import math
from matplotlib.lines import Line2D

# Configuration
CSV_FILE = "benchmark_results.csv"
OUTPUT_IMG = "chart.png"

def plot_zigzag():
    if not os.path.exists(CSV_FILE):
        print(f"Error: {CSV_FILE} not found.")
        return

    # 1. Load and Preprocess
    df = pd.read_csv(CSV_FILE)
    
    # Filter out failures
    df = df[df['Status'] == 'OK']

    # Calculate Space Saved (Higher is better)
    df['Space Saved (%)'] = (1.0 - df['Compression Ratio']) * 100
    
    # Clean names
    df['Dataset'] = df['Dataset Name'].str.replace('dataset_silesia_', '', regex=False).str.replace('.tar', '', regex=False)
    
    # Get unique lists
    datasets = df['Dataset'].unique()
    apps = df['App'].unique()
    
    # 2. Setup Canvas (Grid of Subplots)
    num_plots = len(datasets)
    cols = 3
    rows = math.ceil(num_plots / cols)
    
    # sharex=False, sharey=False ensures every chart scales to its own min/max
    fig, axes = plt.subplots(rows, cols, figsize=(18, 5 * rows), 
                             sharex=False, sharey=False, 
                             constrained_layout=True)
    axes = axes.flatten()

    # Color palette
    palette = sns.color_palette("bright", n_colors=len(apps))
    app_colors = dict(zip(apps, palette))

    print("Generating Zig-Zag plots...")

    for i, dataset in enumerate(datasets):
        ax = axes[i]
        ds_data = df[df['Dataset'] == dataset]

        for app in apps:
            app_data = ds_data[ds_data['App'] == app].sort_values('Level')
            
            if app_data.empty:
                continue

            color = app_colors[app]

            # --- A. Draw Compression Zigzag (Solid Line, Circle) ---
            ax.plot(
                app_data['Compression Speed'], 
                app_data['Space Saved (%)'], 
                color=color, 
                marker='o',          # Circle
                markersize=4,        # Smaller size
                linestyle='-', 
                linewidth=1.5, 
                alpha=0.9,
                label=f"{app}" if i == 0 else ""
            )

            # --- B. Draw Decompression Zigzag (Dashed Line, Triangle) ---
            ax.plot(
                app_data['Decompression Speed'], 
                app_data['Space Saved (%)'], 
                color=color, 
                marker='^',          # Triangle Up
                markersize=4,        # Smaller size
                linestyle='--', 
                linewidth=1.0, 
                alpha=0.7
            )

            # --- C. Draw Thin Connecting Lines (Level to Level) ---
            # Connects Comp Point -> Decomp Point for the same level
            for _, row in app_data.iterrows():
                ax.plot(
                    [row['Compression Speed'], row['Decompression Speed']],
                    [row['Space Saved (%)'], row['Space Saved (%)']],
                    color=color,
                    linewidth=0.5,
                    alpha=0.3,
                    linestyle=':'
                )
                
                # Label Level 1 and 9 to see direction
                # Only label if points aren't completely overlapping
                if row['Level'] == 1:
                    ax.text(row['Compression Speed'], row['Space Saved (%)'], "1", fontsize=7, color=color, fontweight='bold')
                elif row['Level'] == 9:
                    ax.text(row['Compression Speed'], row['Space Saved (%)'], "9", fontsize=7, color=color)

        # Subplot Styling
        ax.set_title(dataset, fontsize=12, fontweight='bold')
        ax.set_xscale('log') 
        ax.grid(True, which="both", ls="-", alpha=0.15)
        ax.set_xlabel("Speed (MB/s)")
        ax.set_ylabel("Space Saved (%)")

    # Turn off empty subplots
    for j in range(i + 1, len(axes)):
        axes[j].axis('off')

    # --- Legend ---
    legend_elements = [
        Line2D([0], [0], color='black', marker='o', markersize=5, lw=1.5, label='Compression'),
        Line2D([0], [0], color='black', marker='^', markersize=5, lw=1.5, linestyle='--', label='Decompression'),
        Line2D([0], [0], color='black', lw=0.5, linestyle=':', label='Link (Same Level)')
    ]
    # Add App Colors to legend
    for app in apps:
        legend_elements.append(Line2D([0], [0], color=app_colors[app], lw=2, label=app))

    fig.legend(handles=legend_elements, loc='upper center', bbox_to_anchor=(0.5, 1.03), ncol=len(apps)+3, fontsize=11)
    
    fig.suptitle("Benchmark: Speed vs Compression Trade-offs", fontsize=18, y=1.06)

    plt.savefig(OUTPUT_IMG, dpi=150, bbox_inches='tight')
    print(f"Zig-Zag plot saved to: {os.path.abspath(OUTPUT_IMG)}")

if __name__ == "__main__":
    plot_zigzag()
