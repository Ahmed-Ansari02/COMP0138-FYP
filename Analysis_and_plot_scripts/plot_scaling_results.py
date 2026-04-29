#!/usr/bin/env python3
"""Generate plots for the WAMR multi-container scaling benchmark."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

try:
    import matplotlib.pyplot as plt
except Exception as exc:
    raise SystemExit(
        "matplotlib is required for plotting. Install with:\n"
        "  python3 -m pip install matplotlib\n"
        f"Import error: {exc}"
    )


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = Path("experiments/ipc_benchmark/scaling/results.csv")
LEGACY_INPUT = Path("ipc_benchmark/results/scaling/results.csv")


def resolve_repo_path(path: Path) -> Path:
    candidate = path.expanduser()
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot container scaling results for interpreter and AOT runs."
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=DEFAULT_INPUT,
        help="Scaling benchmark CSV to read.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("experiments/ipc_benchmark/scaling/plots"),
        help="Directory for generated PNG files.",
    )
    return parser.parse_args()


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as f:
        reader = csv.reader(f)
        rows = list(reader)

    if not rows:
        raise ValueError(f"No data found in {csv_path}")

    header = [cell.strip() for cell in rows[0]]
    return [
        dict(zip(header, [cell.strip() for cell in row]))
        for row in rows[1:]
        if row and any(cell.strip() for cell in row)
    ]


def rows_for_mode(rows: list[dict[str, str]], mode: str) -> list[dict[str, str]]:
    return sorted(
        [row for row in rows if row["mode"] == mode],
        key=lambda row: int(row["container_count"]),
    )


def ok_rows(mode_rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in mode_rows if row["status"] == "ok"]


def first_failed_row(mode_rows: list[dict[str, str]]) -> dict[str, str] | None:
    for row in mode_rows:
        if row["status"] != "ok":
            return row
    return None


def bytes_to_kb(value: str) -> float:
    return int(value) / 1000.0


def us_to_ms(value: str) -> float:
    return int(value) / 1000.0


def annotate_failure(ax, row: dict[str, str] | None, y_value: float, color: str) -> None:
    if not row:
        return

    x = int(row["container_count"])
    label = row["status"].replace("_", " ")
    ax.scatter([x], [y_value], color=color, marker="x", s=70, zorder=5)
    ax.annotate(
        label,
        xy=(x, y_value),
        xytext=(6, 6),
        textcoords="offset points",
        fontsize=8,
        color=color,
    )


def heap_used_kb(row: dict[str, str], free_heap_column: str) -> float:
    return (int(row["free_heap_after_init"]) - int(row[free_heap_column])) / 1000.0


def plot_heap(mode_rows_map: dict[str, list[dict[str, str]]], out_dir: Path) -> Path:
    colors = {"interpreter": "#d95f02", "aot": "#1b9e77"}
    labels = {"interpreter": "Interpreter", "aot": "AOT"}

    fig, ax = plt.subplots(figsize=(9.5, 5.8))

    for mode in ("interpreter", "aot"):
        mode_rows = mode_rows_map[mode]
        successful = ok_rows(mode_rows)
        xs = [int(row["container_count"]) for row in successful]
        after_inst = [
            heap_used_kb(row, "free_heap_after_instantiate") for row in successful
        ]
        steady = [heap_used_kb(row, "free_heap_steady") for row in successful]

        ax.plot(
            xs,
            after_inst,
            marker="o",
            linewidth=1.8,
            color=colors[mode],
            label=f"{labels[mode]} after instantiation",
        )
        ax.plot(
            xs,
            steady,
            marker="s",
            linewidth=1.6,
            linestyle="--",
            color=colors[mode],
            label=f"{labels[mode]} steady state",
        )

        failed = first_failed_row(mode_rows)
        if failed:
            annotate_failure(
                ax,
                failed,
                heap_used_kb(failed, "free_heap_steady"),
                colors[mode],
            )

    ax.set_title("Heap Usage vs Concurrent Container Count")
    ax.set_xlabel("Container count")
    ax.set_ylabel("Additional heap used after WAMR init (kB)")
    ax.set_xticks(range(1, max(int(r["container_count"]) for rows in mode_rows_map.values() for r in rows) + 1))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=8)
    fig.tight_layout()

    out_path = out_dir / "container_scaling_heap.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def plot_cpu(mode_rows_map: dict[str, list[dict[str, str]]], out_dir: Path) -> Path:
    colors = {"interpreter": "#d95f02", "aot": "#1b9e77"}
    labels = {"interpreter": "Interpreter", "aot": "AOT"}

    fig, ax = plt.subplots(figsize=(9.5, 5.4))

    for mode in ("interpreter", "aot"):
        mode_rows = mode_rows_map[mode]
        successful = ok_rows(mode_rows)
        xs = [int(row["container_count"]) for row in successful]
        cpu = [float(row["cpu_overall_mean"]) for row in successful]

        ax.plot(
            xs,
            cpu,
            marker="o",
            linewidth=1.8,
            color=colors[mode],
            label=labels[mode],
        )

        failed = first_failed_row(mode_rows)
        if failed:
            annotate_failure(
                ax,
                failed,
                float(failed["cpu_overall_mean"]),
                colors[mode],
            )

    ax.set_title("CPU Usage vs Concurrent Container Count")
    ax.set_xlabel("Container count")
    ax.set_ylabel("Overall CPU mean (%)")
    ax.set_ylim(bottom=0)
    ax.set_xticks(range(1, max(int(r["container_count"]) for rows in mode_rows_map.values() for r in rows) + 1))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")
    fig.tight_layout()

    out_path = out_dir / "container_scaling_cpu.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def plot_startup(mode_rows_map: dict[str, list[dict[str, str]]], out_dir: Path) -> Path:
    colors = {"interpreter": "#d95f02", "aot": "#1b9e77"}
    labels = {"interpreter": "Interpreter", "aot": "AOT"}

    fig, ax = plt.subplots(figsize=(9.5, 5.4))

    for mode in ("interpreter", "aot"):
        mode_rows = mode_rows_map[mode]
        successful = ok_rows(mode_rows)
        xs = [int(row["container_count"]) for row in successful]
        startup = [us_to_ms(row["startup_time_total_us"]) for row in successful]

        ax.plot(
            xs,
            startup,
            marker="o",
            linewidth=1.8,
            color=colors[mode],
            label=labels[mode],
        )

        failed = first_failed_row(mode_rows)
        if failed:
            annotate_failure(
                ax,
                failed,
                us_to_ms(failed["startup_time_total_us"]),
                colors[mode],
            )

    ax.set_title("Startup Time vs Concurrent Container Count")
    ax.set_xlabel("Container count")
    ax.set_ylabel("Startup time (ms)")
    ax.set_xticks(range(1, max(int(r["container_count"]) for rows in mode_rows_map.values() for r in rows) + 1))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")

    fig.tight_layout()

    out_path = out_dir / "container_scaling_startup.png"
    fig.savefig(out_path, dpi=180)
    plt.close(fig)
    return out_path


def main() -> None:
    args = parse_args()
    input_path = resolve_repo_path(args.input)
    if not input_path.exists() and args.input == DEFAULT_INPUT:
        legacy_path = resolve_repo_path(LEGACY_INPUT)
        if legacy_path.exists():
            input_path = legacy_path

    rows = load_rows(input_path)

    mode_rows_map = {
        "interpreter": rows_for_mode(rows, "interpreter"),
        "aot": rows_for_mode(rows, "aot"),
    }

    output_dir = resolve_repo_path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    heap_path = plot_heap(mode_rows_map, output_dir)
    cpu_path = plot_cpu(mode_rows_map, output_dir)
    startup_path = plot_startup(mode_rows_map, output_dir)

    print(f"[OK] Loaded: {input_path}")
    print(f"[OK] Saved: {heap_path}")
    print(f"[OK] Saved: {cpu_path}")
    print(f"[OK] Saved: {startup_path}")


if __name__ == "__main__":
    main()
