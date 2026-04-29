#!/usr/bin/env python3
"""Graph CPU usage for native and WAMR controller runs."""

from __future__ import annotations

import argparse
from pathlib import Path

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


REPO_ROOT = Path(__file__).resolve().parents[1]


def resolve_repo_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def latest_run_dir(base_dir: Path) -> Path:
    run_dirs = sorted(path for path in base_dir.iterdir() if path.is_dir())
    if not run_dirs:
        raise FileNotFoundError(f"No run directories found under {base_dir}")
    return run_dirs[-1]


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot CPU usage for native and WAMR experiment CSVs."
    )
    parser.add_argument(
        "--base",
        default="experiments/cpu_investigation",
        help="Base directory containing timestamped CPU-investigation runs.",
    )
    parser.add_argument(
        "--run",
        default="",
        help="Specific CPU-investigation run directory. Default: latest run under --base.",
    )
    parser.add_argument("--native", default="", help="Explicit native results CSV.")
    parser.add_argument("--wamr", default="", help="Explicit WAMR results CSV.")
    parser.add_argument(
        "--output-dir",
        default="",
        help="Directory for the PNG file. Default: the selected run's plots directory.",
    )
    parser.add_argument("--show", action="store_true", help="Show the plot interactively.")
    return parser.parse_args()


def load_cpu_csv(path: Path) -> pd.DataFrame:
    df = pd.read_csv(path)
    if "index" not in df.columns:
        df["index"] = range(len(df))
    return df


def main() -> None:
    args = parse_args()

    run_dir = None
    if args.native and args.wamr:
        native_csv = resolve_repo_path(args.native)
        wamr_csv = resolve_repo_path(args.wamr)
    else:
        base_dir = resolve_repo_path(args.base)
        run_dir = resolve_run_dir(args.run, base_dir)
        native_csv = run_dir / "native_baseline" / "results.csv"
        wamr_csv = run_dir / "wamr_interp_core0" / "results.csv"

    output_dir = (
        resolve_repo_path(args.output_dir)
        if args.output_dir
        else (run_dir / "plots" if run_dir else REPO_ROOT / "experiments" / "cpu_investigation" / "plots")
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    native = load_cpu_csv(native_csv)
    wamr = load_cpu_csv(wamr_csv)

    native_cpu = native[["index", "cpu_core0", "cpu_core1", "cpu_overall"]].dropna()
    wamr_cpu = wamr[["index", "cpu_core0", "cpu_core1", "cpu_overall"]].dropna()

    metrics = [
        ("cpu_core0", "CPU Core 0 Usage"),
        ("cpu_core1", "CPU Core 1 Usage"),
        ("cpu_overall", "CPU Overall Usage"),
    ]

    skip = 5
    native_cpu = native_cpu[native_cpu["index"] >= skip].copy()
    wamr_cpu = wamr_cpu[wamr_cpu["index"] >= skip].copy()

    fig, axes = plt.subplots(3, 1, figsize=(12, 10), sharex=False)

    for ax, (col, title) in zip(axes, metrics):
        ax.plot(native_cpu["index"], native_cpu[col], label="Native", alpha=0.8, linewidth=0.8)
        ax.plot(wamr_cpu["index"], wamr_cpu[col], label="WAMR", alpha=0.8, linewidth=0.8)
        ax.set_title(title)
        ax.set_ylabel("Usage (%)")
        ax.legend()
        ax.grid(True, alpha=0.3)

        all_vals = pd.concat([native_cpu[col], wamr_cpu[col]])
        ymin = all_vals.quantile(0.01)
        ymax = all_vals.quantile(0.99)
        margin = max((ymax - ymin) * 0.2, 0.05)
        ax.set_ylim(ymin - margin, ymax + margin)

        n_mean = native_cpu[col].mean()
        w_mean = wamr_cpu[col].mean()
        n_std = native_cpu[col].std()
        w_std = wamr_cpu[col].std()
        stats_text = f"Native: {n_mean:.2f}% +/- {n_std:.2f}%\nWAMR:  {w_mean:.2f}% +/- {w_std:.2f}%"
        ax.text(
            0.98,
            0.95,
            stats_text,
            transform=ax.transAxes,
            fontsize=8,
            verticalalignment="top",
            horizontalalignment="right",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="wheat", alpha=0.5),
        )

    axes[-1].set_xlabel("Sample Index")
    fig.suptitle("WAMR vs Native CPU Usage Comparison", fontsize=14, fontweight="bold")
    fig.tight_layout()

    out_path = output_dir / "cpu_usage_comparison.png"
    fig.savefig(out_path, dpi=150)
    print(f"Native CSV: {native_csv}")
    print(f"WAMR CSV:   {wamr_csv}")
    print(f"Saved: {out_path}")

    if args.show:
        plt.show()
    plt.close(fig)


if __name__ == "__main__":
    main()
