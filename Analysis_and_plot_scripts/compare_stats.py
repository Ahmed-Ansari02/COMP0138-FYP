#!/usr/bin/env python3
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
    import numpy as np
except Exception as exc:
    raise SystemExit(
        "numpy is required for plotting. Install with:\n"
        "  python3 -m pip install numpy\n"
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


def default_paths(run_dir: Path, algo: str, mode: str) -> tuple[Path, Path]:
    native_path = run_dir / "native" / algo / "results.csv"
    wasm_candidates = [
        run_dir / "wamr" / mode / algo / "results.csv",
        run_dir / "wasm" / algo / "results.csv",
    ]
    for wasm_path in wasm_candidates:
        if wasm_path.exists():
            return native_path, wasm_path
    return native_path, wasm_candidates[0]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare native and WAMR timing metrics from experiment CSVs."
    )
    parser.add_argument(
        "--base",
        default="experiments/control_algorithms",
        help="Base experiment directory containing timestamped control-algorithm runs.",
    )
    parser.add_argument(
        "--run",
        default="",
        help="Specific control-algorithm run directory. Default: latest run under --base.",
    )
    parser.add_argument(
        "--algo",
        default="bang_bang",
        choices=("bang_bang", "pid", "e2e"),
        help="Control algorithm to compare when --native and --wasm are not supplied.",
    )
    parser.add_argument(
        "--mode",
        default="wasm",
        choices=("wasm", "aot"),
        help="WAMR execution mode to compare against native.",
    )
    parser.add_argument("--native", default="", help="Explicit native results CSV.")
    parser.add_argument("--wasm", default="", help="Explicit WAMR results CSV.")
    parser.add_argument(
        "--output-dir",
        default="",
        help="Directory for the comparison plot. Default: the selected run's plots directory.",
    )
    return parser.parse_args()


def load_data(filepath: Path) -> pd.DataFrame | None:
    """Load results CSV and standardize column names if needed."""
    try:
        df = pd.read_csv(filepath)
        rename_map = {
            "algo_time_us": "exec_time_us",
            "wasm_time_us": "exec_time_us",
            "set_heater_time_us": "heater_time_us",
            "get_temperature_time_us": "temp_get_time_us",
            "temperature_get_time_us": "temp_get_time_us",
        }
        df.rename(columns=rename_map, inplace=True)
        return df
    except Exception as exc:
        print(f"Error loading {filepath}: {exc}")
        return None


def main() -> None:
    args = parse_args()

    run_dir = None
    if args.native and args.wasm:
        native_path = resolve_repo_path(args.native)
        wasm_path = resolve_repo_path(args.wasm)
    else:
        base_dir = resolve_repo_path(args.base)
        run_dir = resolve_run_dir(args.run, base_dir)
        native_path, wasm_path = default_paths(run_dir, args.algo, args.mode)

    output_dir = (
        resolve_repo_path(args.output_dir)
        if args.output_dir
        else (run_dir / "plots" if run_dir else REPO_ROOT / "experiments" / "comparison" / "plots")
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    df_native = load_data(native_path)
    df_wasm = load_data(wasm_path)

    if df_native is None or df_wasm is None:
        print("Could not load both datasets.")
        print(f"Native CSV: {native_path}")
        print(f"WAMR CSV:   {wasm_path}")
        return

    metrics = [
        "exec_time_us",
        "loop_time_us",
        "adc_time_us",
        "heater_time_us",
        "temp_get_time_us",
        "e2e_time_us",
    ]
    labels = ["Exec", "Loop", "ADC", "Heater", "Temp Get", "E2E"]

    print(f"\nNative CSV: {native_path}")
    print(f"WAMR CSV:   {wasm_path}\n")
    print("=" * 95)
    print(f"{'Metric':<12} | {'Native (Mean)':<12} | {'WAMR (Mean)':<12} | {'Diff (us)':<10} | {'Overhead (%)':<12}")
    print("-" * 95)

    native_means = []
    wasm_means = []

    for metric, label in zip(metrics, labels):
        if metric in df_native.columns and metric in df_wasm.columns:
            n_val = df_native[metric].mean()
            w_val = df_wasm[metric].mean()
            diff = w_val - n_val
            pct = ((w_val - n_val) / n_val) * 100 if n_val != 0 else 0.0

            native_means.append(n_val)
            wasm_means.append(w_val)

            print(f"{label:<12} | {n_val:>12.2f} | {w_val:>12.2f} | {diff:>10.2f} | {pct:>11.1f}%")
        else:
            native_means.append(0)
            wasm_means.append(0)
            print(f"{label:<12} | {'N/A':>12} | {'N/A':>12} | {'N/A':>10} | {'N/A':>12}")

    print("=" * 95 + "\n")

    x = np.arange(len(labels))
    width = 0.35

    fig, ax = plt.subplots(figsize=(10, 6))
    rects1 = ax.bar(x - width / 2, native_means, width, label="Native", color="tab:blue")
    rects2 = ax.bar(x + width / 2, wasm_means, width, label="WAMR", color="tab:orange")

    ax.set_ylabel("Time (microseconds)")
    ax.set_title(f"Performance Comparison: Native vs WAMR ({args.mode.upper()}, {args.algo})")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend()

    ax.bar_label(rects1, fmt="%.0f", padding=3, rotation=90)
    ax.bar_label(rects2, fmt="%.0f", padding=3, rotation=90)

    plt.tight_layout()
    output_file = output_dir / f"comparison_{args.mode}_{args.algo}.png"
    plt.savefig(output_file, dpi=150)
    print(f"Comparison plot saved to: {output_file}")


if __name__ == "__main__":
    main()
