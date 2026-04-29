import sys
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def resolve_repo_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def graph_output_dir(csv_path: Path) -> Path:
    try:
        relative = csv_path.relative_to(REPO_ROOT)
    except ValueError:
        return REPO_ROOT / "experiments" / "analysis" / csv_path.stem / "graphs"

    if relative.parts and relative.parts[0] == "experiments":
        return csv_path.parent / "graphs"

    return REPO_ROOT / "experiments" / "analysis" / csv_path.stem / "graphs"


def load_data(filepath: str) -> pd.DataFrame:
    """Load the results CSV file."""
    df = pd.read_csv(filepath)
    return df

def compute_statistics(df: pd.DataFrame) -> dict:
    """Compute statistics for each timing column."""
    timing_columns = df.columns.to_list()
    stats = {}

    for col in timing_columns:
        if col in df.columns:
            data = df[col].dropna()
            if len(data) > 0:
                stats[col] = {
                    'count': len(data),
                    'mean': data.mean(),
                    'std': data.std(),
                    'min': data.min(),
                    'max': data.max(),
                    'median': data.median(),
                    'p95': data.quantile(0.95),
                    'p99': data.quantile(0.99),
                }
    return stats

def print_statistics(stats: dict):
    """Print statistics in a formatted table."""
    print("\n" + "=" * 80)
    print("TIMING STATISTICS (microseconds)")
    print("=" * 80)

    headers = ['Metric', 'Count', 'Mean', 'Std', 'Min', 'Max', 'Median', 'P95', 'P99']
    print(f"{headers[0]:<20} {headers[1]:>8} {headers[2]:>10} {headers[3]:>10} {headers[4]:>10} {headers[5]:>10} {headers[6]:>10} {headers[7]:>10} {headers[8]:>10}")
    print("-" * 80)

    for metric, values in stats.items():
        name = metric.replace('_time_us', '').replace('_', ' ').title()
        print(f"{name:<20} {values['count']:>8} {values['mean']:>10.2f} {values['std']:>10.2f} "
              f"{values['min']:>10.2f} {values['max']:>10.2f} {values['median']:>10.2f} "
              f"{values['p95']:>10.2f} {values['p99']:>10.2f}")

def plot_histograms(df: pd.DataFrame, output_dir: str = "."):
    """Plot histograms for each timing metric."""
    timing_columns = ['algo_time_us', 'loop_time_us', 'adc_time_us', 'heater_time_us', 'e2e_time_us']
    available_cols = [col for col in timing_columns if col in df.columns and df[col].notna().sum() > 0]

    if not available_cols:
        print("No timing data available to plot.")
        return

    fig, axes = plt.subplots(len(available_cols), 1, figsize=(10, 3 * len(available_cols)))
    if len(available_cols) == 1:
        axes = [axes]

    for ax, col in zip(axes, available_cols):
        data = df[col].dropna()
        ax.hist(data, bins=50, edgecolor='black', alpha=0.7)
        ax.set_xlabel('Time (us)')
        ax.set_ylabel('Frequency')
        ax.set_title(f'{col.replace("_time_us", "").replace("_", " ").title()} Timing Distribution')
        ax.axvline(data.mean(), color='r', linestyle='--', label=f'Mean: {data.mean():.2f}us')
        ax.axvline(data.median(), color='g', linestyle='--', label=f'Median: {data.median():.2f}us')
        ax.legend()

    plt.tight_layout()
    output_path = Path(output_dir) / "timing_histograms.png"
    plt.savefig(output_path, dpi=150)
    print(f"\nHistograms saved to: {output_path}")
    plt.close()

def plot_time_series(df: pd.DataFrame, output_dir: str = "."):
    """Plot time series of timing metrics."""
    timing_columns = ['algo_time_us', 'loop_time_us', 'adc_time_us', 'heater_time_us', 'e2e_time_us']
    available_cols = [col for col in timing_columns if col in df.columns and df[col].notna().sum() > 0]

    if not available_cols:
        print("No timing data available to plot.")
        return

    fig, ax = plt.subplots(figsize=(12, 6))

    for col in available_cols:
        data = df[col].dropna()
        ax.plot(data.index, data.values, label=col.replace("_time_us", "").replace("_", " ").title(), alpha=0.7)

    ax.set_xlabel('Sample Index')
    ax.set_ylabel('Time (us)')
    ax.set_title('Timing Metrics Over Time')
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    output_path = Path(output_dir) / "timing_series.png"
    plt.savefig(output_path, dpi=150)
    print(f"Time series saved to: {output_path}")
    plt.close()

def plot_boxplots(df: pd.DataFrame, output_dir: str = "."):
    """Plot boxplots for timing comparison."""
    timing_columns = ['algo_time_us', 'loop_time_us', 'adc_time_us', 'heater_time_us', 'e2e_time_us']
    available_cols = [col for col in timing_columns if col in df.columns and df[col].notna().sum() > 0]

    if not available_cols:
        print("No timing data available to plot.")
        return

    fig, ax = plt.subplots(figsize=(10, 6))

    data_to_plot = [df[col].dropna().values for col in available_cols]
    labels = [col.replace("_time_us", "").replace("_", " ").title() for col in available_cols]

    ax.boxplot(data_to_plot, labels=labels)
    ax.set_ylabel('Time (us)')
    ax.set_title('Timing Metrics Comparison')
    ax.grid(True, alpha=0.3, axis='y')

    plt.tight_layout()
    output_path = Path(output_dir) / "timing_boxplots.png"
    plt.savefig(output_path, dpi=150)
    print(f"Boxplots saved to: {output_path}")
    plt.close()

def plot_all_metrics_linegraphs(df: pd.DataFrame, output_dir: str = "."):
    """Plot a line graph for every numeric metric with auto-scaled y-axis."""
    numeric_cols = df.select_dtypes(include=[np.number]).columns
    available_cols = [col for col in numeric_cols if df[col].notna().sum() > 0]
    if not available_cols:
        print("No numeric data available to plot.")
        return

    n = len(available_cols)
    fig, axes = plt.subplots(n, 1, figsize=(12, 3 * n), sharex=True)
    if n == 1:
        axes = [axes]

    for ax, col in zip(axes, available_cols):
        data = df[col].dropna()
        ax.plot(data.index, data.values, label=col, color='tab:blue')
        ymin, ymax = data.min(), data.max()
        margin = (ymax - ymin) * 0.05 if ymax > ymin else 1
        ax.set_ylim(ymin - margin, ymax + margin)
        ax.set_ylabel(col)
        ax.set_title(f"{col} (min={ymin:.2f}, max={ymax:.2f})")
        ax.grid(True, alpha=0.3)
        ax.legend()

    axes[-1].set_xlabel("Sample Index")
    plt.tight_layout()
    output_path = Path(output_dir) / "all_metrics_linegraphs.png"
    plt.savefig(output_path, dpi=150)
    print(f"Line graphs saved to: {output_path}")
    plt.close()

def main():
    """Main analysis function."""
    if len(sys.argv) < 2:
        print("Usage: python3 analyse.py <csv_path>")
        sys.exit(1)

    csv_path = resolve_repo_path(sys.argv[1])

    if not csv_path.exists():
        print(f"Error: {csv_path} not found!")
        return

    storage_dir = graph_output_dir(csv_path)
    storage_dir.mkdir(parents=True, exist_ok=True)

    print(f"Loading data from: {csv_path}")
    df = load_data(str(csv_path))

    print(f"Loaded {len(df)} rows")
    print(f"Columns: {list(df.columns)}")

    # Compute and print statistics
    stats = compute_statistics(df)
    print_statistics(stats)

    # Generate plots
    print("\nGenerating plots...")
    plot_histograms(df, str(storage_dir))
    plot_time_series(df, str(storage_dir))
    plot_boxplots(df, str(storage_dir))
    plot_all_metrics_linegraphs(df, str(storage_dir))

    print(f"\nAnalysis complete! Graphs saved to: {storage_dir}")

if __name__ == "__main__":
    main()
