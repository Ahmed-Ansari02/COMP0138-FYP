#!/usr/bin/env python3
"""Plot control algorithm metrics across iteration for native/WAMR runs."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List

try:
    import matplotlib.pyplot as plt
except Exception as exc:
    raise SystemExit(
        "matplotlib is required for plotting. Install with:\n"
        "  python3 -m pip install matplotlib\n"
        f"Import error: {exc}"
    )

try:
    import pandas as pd
except Exception as exc:
    raise SystemExit(
        "pandas is required for CSV handling. Install with:\n"
        "  python3 -m pip install pandas\n"
        f"Import error: {exc}"
    )

ALGORITHMS = ("bang_bang", "pid", "e2e")
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_METRICS = [
    "loop_time_us",
    "adc_time_us",
    "heater_time_us",
    "temp_get_time_us",
    "e2e_time_us",
    "cpu_overall",
    "free_heap",
]
TEMP_COLUMN_CANDIDATES = [
    "temperature_c",
    "temperature",
    "temp_c",
    "temp",
    "current_temp",
    "measured_temp",
    "temp_get_time_us",  # fallback: timing metric, not actual temperature value
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot control algorithm metrics vs iteration for each run variant."
    )
    parser.add_argument(
        "--run",
        type=str,
        default="",
        help="Run directory under experiments/control_algorithms/<timestamp>. Default: latest.",
    )
    parser.add_argument(
        "--base",
        type=str,
        default="experiments/control_algorithms",
        help="Base directory containing timestamp run folders.",
    )
    parser.add_argument(
        "--metrics",
        type=str,
        default=",".join(DEFAULT_METRICS),
        help="Comma-separated metrics to plot.",
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="Show plots interactively as well as save PNG files.",
    )
    parser.add_argument(
        "--temp-column",
        type=str,
        default="",
        help="Optional explicit temperature column to use for temp-vs-iteration plots.",
    )
    return parser.parse_args()


def latest_run_dir(base_dir: Path) -> Path:
    run_dirs = sorted([p for p in base_dir.iterdir() if p.is_dir()])
    if not run_dirs:
        raise FileNotFoundError(f"No run directories found in {base_dir}")
    return run_dirs[-1]


def resolve_repo_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def resolve_run_dir(run_arg: str, base_dir: Path) -> Path:
    if not run_arg:
        return latest_run_dir(base_dir)

    direct = resolve_repo_path(run_arg)
    if direct.is_dir():
        return direct

    by_timestamp = base_dir / run_arg
    if by_timestamp.is_dir():
        return by_timestamp

    return direct


def discover_variants(run_dir: Path) -> Dict[str, Path]:
    candidates = {
        "native": run_dir / "native",
        "wasm": run_dir / "wasm",  # legacy layout
        "wamr_wasm": run_dir / "wamr" / "wasm",
        "wamr_aot": run_dir / "wamr" / "aot",
    }
    return {name: path for name, path in candidates.items() if path.is_dir()}


def load_algo_csv(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    if "index" not in df.columns:
        df["index"] = range(len(df))

    for col in df.columns:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    return df


def plot_variant(
    variant_name: str,
    variant_dir: Path,
    metrics: List[str],
    out_dir: Path,
    show: bool,
) -> None:
    algo_frames: Dict[str, pd.DataFrame] = {}

    for algo in ALGORITHMS:
        csv_path = variant_dir / algo / "results.csv"
        if not csv_path.exists():
            continue
        try:
            algo_frames[algo] = load_algo_csv(csv_path)
        except Exception as exc:
            print(f"[WARN] Failed to load {csv_path}: {exc}")

    if not algo_frames:
        print(f"[WARN] No algorithm CSVs found for variant: {variant_name}")
        return

    available_metrics: List[str] = []
    for metric in metrics:
        if any(metric in df.columns and df[metric].notna().sum() > 1 for df in algo_frames.values()):
            available_metrics.append(metric)

    if not available_metrics:
        print(f"[WARN] No requested metrics with data for {variant_name}")
        return

    cols = 2
    rows = (len(available_metrics) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(14, max(4 * rows, 6)))
    axes = axes.flatten() if hasattr(axes, "flatten") else [axes]

    for i, metric in enumerate(available_metrics):
        ax = axes[i]
        for algo in ALGORITHMS:
            df = algo_frames.get(algo)
            if df is None or metric not in df.columns:
                continue
            valid = df[["index", metric]].dropna()
            if len(valid) <= 1:
                continue
            ax.plot(valid["index"], valid[metric], linewidth=1.2, alpha=0.9, label=algo)

        ax.set_title(metric)
        ax.set_xlabel("iteration")
        ax.set_ylabel(metric)
        ax.grid(True, alpha=0.3)
        ax.legend()

    for j in range(len(available_metrics), len(axes)):
        fig.delaxes(axes[j])

    fig.suptitle(f"Control Algorithms vs Iteration ({variant_name})", fontsize=13)
    fig.tight_layout()

    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / f"{variant_name}_algorithms_vs_iteration.png"
    fig.savefig(out_file, dpi=160)
    print(f"[OK] Saved: {out_file}")

    if show:
        plt.show()
    plt.close(fig)


def resolve_temp_column(
    algo_frames: Dict[str, pd.DataFrame], explicit_temp_col: str
) -> str:
    if explicit_temp_col:
        return explicit_temp_col

    for candidate in TEMP_COLUMN_CANDIDATES:
        if any(candidate in df.columns and df[candidate].notna().sum() > 1 for df in algo_frames.values()):
            return candidate

    return ""


def plot_temp_per_method(
    variant_name: str,
    algo_frames: Dict[str, pd.DataFrame],
    out_dir: Path,
    temp_col: str,
    show: bool,
) -> None:
    if not temp_col:
        print(f"[WARN] No temperature-like column found for {variant_name}; skipped temp plots.")
        return

    if temp_col == "temp_get_time_us":
        print(
            f"[WARN] Using '{temp_col}' for {variant_name}. "
            "This is temperature read timing, not actual temperature."
        )

    for algo in ALGORITHMS:
        df = algo_frames.get(algo)
        if df is None or temp_col not in df.columns:
            continue

        valid = df[["index", temp_col]].dropna()
        if len(valid) <= 1:
            continue

        fig, ax = plt.subplots(figsize=(10, 5))
        ax.plot(valid["index"], valid[temp_col], linewidth=1.3, alpha=0.95)
        ax.set_title(f"{variant_name} - {algo}: {temp_col} vs iteration")
        ax.set_xlabel("iteration")
        ax.set_ylabel(temp_col)
        ax.grid(True, alpha=0.3)
        fig.tight_layout()

        out_dir.mkdir(parents=True, exist_ok=True)
        out_file = out_dir / f"{variant_name}_{algo}_temp_vs_iteration.png"
        fig.savefig(out_file, dpi=170)
        print(f"[OK] Saved: {out_file}")

        if show:
            plt.show()
        plt.close(fig)


def main() -> None:
    args = parse_args()

    base_dir = resolve_repo_path(args.base)
    if not base_dir.is_dir():
        raise FileNotFoundError(f"Base directory not found: {base_dir}")

    run_dir = resolve_run_dir(args.run, base_dir)
    if not run_dir.is_dir():
        raise FileNotFoundError(f"Run directory not found: {run_dir}")

    metrics = [m.strip() for m in args.metrics.split(",") if m.strip()]
    if not metrics:
        raise ValueError("No metrics specified.")

    print(f"[INFO] Run: {run_dir}")
    print(f"[INFO] Metrics: {', '.join(metrics)}")

    variants = discover_variants(run_dir)
    if not variants:
        raise FileNotFoundError(
            f"No variants found in {run_dir}. Expected native/wasm or wamr/wasm and wamr/aot."
        )

    output_dir = run_dir / "plots"
    for variant_name, variant_dir in variants.items():
        print(f"[INFO] Variant: {variant_name}")
        algo_frames: Dict[str, pd.DataFrame] = {}
        for algo in ALGORITHMS:
            csv_path = variant_dir / algo / "results.csv"
            if not csv_path.exists():
                continue
            try:
                algo_frames[algo] = load_algo_csv(csv_path)
            except Exception as exc:
                print(f"[WARN] Failed to load {csv_path}: {exc}")

        if not algo_frames:
            print(f"[WARN] No algorithm CSVs found for variant: {variant_name}")
            continue

        plot_variant(variant_name, variant_dir, metrics, output_dir, args.show)
        temp_col = resolve_temp_column(algo_frames, args.temp_column.strip())
        plot_temp_per_method(variant_name, algo_frames, output_dir, temp_col, args.show)

    print(f"[DONE] Plots saved under: {output_dir}")


if __name__ == "__main__":
    main()
