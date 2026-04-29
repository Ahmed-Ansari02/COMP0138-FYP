#!/usr/bin/env python3
"""
Phase 3: OTA-like Update Benchmark

Compares three update strategies:
  1. Full flash     — idf.py flash (bootloader + app + partition table + SPIFFS)
  2. SPIFFS-only    — esptool.py write_flash 0x210000 storage.bin (just the WASM containers)
  3. Build + flash  — idf.py build flash (full rebuild + flash)

For each strategy, measures:
  - Flash time (seconds)
  - Data written (bytes)
  - Boot-to-running time (reset → first WASM host function call)

Usage:
    cd Wasm_controller
    python3 ../Ota_benchmarks/ota_benchmark.py [--port /dev/cu.usbserial-XXXX] [--runs 3]
"""

import subprocess
import sys
import os
import time
import re
import argparse
from pathlib import Path
import serial
import serial.tools.list_ports


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "experiments" / "fault_tolerance_test" / "ota_benchmark_results.csv"


def resolve_repo_path(path):
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return (REPO_ROOT / candidate).resolve()


def find_esp32_port():
    """Auto-detect ESP32 serial port."""
    for p in serial.tools.list_ports.comports():
        if "usbserial" in p.device or "USB" in p.device or "CP210" in p.description:
            return p.device
    return None


def time_command(cmd, cwd=None, description=""):
    """Run a command and return (elapsed_seconds, success)."""
    print(f"  [{description}] Running: {' '.join(cmd)}")
    start = time.time()
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    elapsed = time.time() - start
    success = result.returncode == 0
    if not success:
        print(f"  [ERROR] {result.stderr[-500:]}")
    else:
        print(f"  [{description}] Done in {elapsed:.2f}s")
    return elapsed, success


def measure_boot_time(port, baud=115200, timeout=30):
    """
    Reset ESP32 via RTS and measure time from reset to first WASM log line.
    Returns (boot_time_seconds, first_log_line).
    """
    ser = serial.Serial(port, baud, timeout=1)

    # Toggle RTS to reset ESP32
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    ser.dtr = False

    reset_time = time.time()
    ser.reset_input_buffer()

    # Wait for first WASM activity indicator
    markers = [
        "Starting WASM Control Module",
        "WASM:",              # host_log output
        "Loading initial container",
        "FAULT TEST: Loading",
    ]

    while time.time() - reset_time < timeout:
        try:
            line = ser.readline().decode('utf-8', errors='replace')
        except Exception:
            continue
        if not line:
            continue

        # Strip ANSI
        clean = re.sub(r'\x1b\[[0-9;]*m', '', line).strip()

        for marker in markers:
            if marker in clean:
                boot_time = time.time() - reset_time
                ser.close()
                return boot_time, clean

    ser.close()
    return timeout, "TIMEOUT"


def get_file_size(path):
    """Get file size in bytes, return 0 if not found."""
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def main():
    parser = argparse.ArgumentParser(description="OTA-like Update Benchmark")
    parser.add_argument("--port", default=None, help="Serial port (auto-detect if omitted)")
    parser.add_argument("--runs", type=int, default=3, help="Number of runs per strategy (default: 3)")
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT), help="Output CSV path")
    parser.add_argument("--skip-build", action="store_true", help="Skip the build+flash strategy")
    args = parser.parse_args()

    port = args.port or find_esp32_port()
    if not port:
        print("ERROR: No ESP32 serial port found. Use --port to specify.")
        sys.exit(1)
    print(f"Using port: {port}")

    output_path = resolve_repo_path(args.output)

    # Prefer the current directory for backwards compatibility, but allow the
    # script to be launched from the repository root after the folder cleanup.
    build_dir = Path.cwd()
    if not (build_dir / "build" / "storage.bin").exists():
        candidate = REPO_ROOT / "Wasm_controller"
        if (candidate / "build" / "storage.bin").exists():
            build_dir = candidate
        else:
            print("ERROR: Could not find Wasm_controller/build/storage.bin")
            sys.exit(1)

    storage_bin = os.path.join(build_dir, "build", "storage.bin")
    app_bin = os.path.join(build_dir, "build", "Wasm_controller.bin")
    bootloader_bin = os.path.join(build_dir, "build", "bootloader", "bootloader.bin")
    partition_bin = os.path.join(build_dir, "build", "partition_table", "partition-table.bin")

    # Sizes
    storage_size = get_file_size(storage_bin)
    full_flash_size = (
        get_file_size(bootloader_bin)
        + get_file_size(partition_bin)
        + get_file_size(app_bin)
        + storage_size
    )

    print(f"\nBinary sizes:")
    print(f"  SPIFFS (storage.bin):        {storage_size:>10,} bytes ({storage_size/1024:.1f} KB)")
    print(f"  Full flash (all binaries):   {full_flash_size:>10,} bytes ({full_flash_size/1024:.1f} KB)")
    print(f"  Ratio:                       {full_flash_size/storage_size:.1f}x\n")

    results = []

    for run in range(1, args.runs + 1):
        print(f"\n{'='*60}")
        print(f"  RUN {run}/{args.runs}")
        print(f"{'='*60}")

        # --- Strategy 1: Full flash (idf.py flash, no build) ---
        print(f"\n--- Strategy 1: Full flash (idf.py flash -p {port}) ---")
        flash_time, ok = time_command(
            ["idf.py", "-p", port, "flash"],
            cwd=build_dir,
            description="Full flash"
        )
        if ok:
            boot_time, first_line = measure_boot_time(port)
            print(f"  Boot-to-running: {boot_time:.2f}s — {first_line[:80]}")
            results.append({
                "run": run,
                "strategy": "full_flash",
                "flash_time_s": round(flash_time, 3),
                "boot_time_s": round(boot_time, 3),
                "total_update_s": round(flash_time + boot_time, 3),
                "bytes_written": full_flash_size,
            })

        # --- Strategy 2: SPIFFS-only flash ---
        print(f"\n--- Strategy 2: SPIFFS-only flash ---")
        spiffs_time, ok = time_command(
            [
                "esptool.py", "--chip", "esp32",
                "--port", port,
                "--baud", "460800",
                "write_flash", "0x210000", storage_bin
            ],
            cwd=build_dir,
            description="SPIFFS flash"
        )
        if ok:
            boot_time, first_line = measure_boot_time(port)
            print(f"  Boot-to-running: {boot_time:.2f}s — {first_line[:80]}")
            results.append({
                "run": run,
                "strategy": "spiffs_only",
                "flash_time_s": round(spiffs_time, 3),
                "boot_time_s": round(boot_time, 3),
                "total_update_s": round(spiffs_time + boot_time, 3),
                "bytes_written": storage_size,
            })

        # --- Strategy 3: Full clean + Build + flash (optional) ---
        if not args.skip_build:
            print(f"\n--- Strategy 3: Full clean + Build + Flash ---")
            # Clean first so build is from scratch (fair comparison vs reflash)
            time_command(
                ["idf.py", "fullclean"],
                cwd=build_dir,
                description="Fullclean"
            )
            build_flash_time, ok = time_command(
                ["idf.py", "-p", port, "build", "flash"],
                cwd=build_dir,
                description="Build+flash"
            )
            if ok:
                boot_time, first_line = measure_boot_time(port)
                print(f"  Boot-to-running: {boot_time:.2f}s — {first_line[:80]}")
                results.append({
                    "run": run,
                    "strategy": "build_and_flash",
                    "flash_time_s": round(build_flash_time, 3),
                    "boot_time_s": round(boot_time, 3),
                    "total_update_s": round(build_flash_time + boot_time, 3),
                    "bytes_written": full_flash_size,
                })

    # --- Write results CSV ---
    if results:
        print(f"\n{'='*60}")
        print(f"  RESULTS")
        print(f"{'='*60}\n")

        header = "run,strategy,flash_time_s,boot_time_s,total_update_s,bytes_written"
        csv_lines = [header]
        for r in results:
            csv_lines.append(f"{r['run']},{r['strategy']},{r['flash_time_s']},{r['boot_time_s']},{r['total_update_s']},{r['bytes_written']}")

        csv_text = "\n".join(csv_lines) + "\n"
        print(csv_text)

        output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_path, 'w') as f:
            f.write(csv_text)
        print(f"Results saved to {output_path}")

        # Summary statistics
        print(f"\n--- Summary (averages across {args.runs} runs) ---")
        for strategy in ["full_flash", "spiffs_only", "build_and_flash"]:
            strat_results = [r for r in results if r["strategy"] == strategy]
            if strat_results:
                avg_flash = sum(r["flash_time_s"] for r in strat_results) / len(strat_results)
                avg_boot = sum(r["boot_time_s"] for r in strat_results) / len(strat_results)
                avg_total = sum(r["total_update_s"] for r in strat_results) / len(strat_results)
                bytes_w = strat_results[0]["bytes_written"]
                print(f"  {strategy:20s}: flash={avg_flash:.2f}s  boot={avg_boot:.2f}s  total={avg_total:.2f}s  data={bytes_w/1024:.0f}KB")

        # Speedup
        full_results = [r for r in results if r["strategy"] == "full_flash"]
        spiffs_results = [r for r in results if r["strategy"] == "spiffs_only"]
        if full_results and spiffs_results:
            avg_full = sum(r["total_update_s"] for r in full_results) / len(full_results)
            avg_spiffs = sum(r["total_update_s"] for r in spiffs_results) / len(spiffs_results)
            print(f"\n  SPIFFS-only speedup: {avg_full/avg_spiffs:.1f}x faster than full flash")
            print(f"  Data reduction: {full_results[0]['bytes_written']/spiffs_results[0]['bytes_written']:.1f}x less data")
    else:
        print("\nNo successful results collected.")


if __name__ == "__main__":
    main()
