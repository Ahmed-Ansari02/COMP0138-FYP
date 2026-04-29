#!/bin/bash
#
# run_scaling_benchmark.bash
# Builds the low-memory scaling worker, packages both .wasm and .aot into a
# dedicated SPIFFS image, then builds/flashes the scaling benchmark firmware.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONTAINER_DIR="$SCRIPT_DIR/containers"
SCALING_DIR="$SCRIPT_DIR/wasm_assets/scaling"
RESULTS_DIR="$ROOT_DIR/experiments/ipc_benchmark/scaling"
EXEC_SCRIPT="$SCRIPT_DIR/../exec_esp32.py"
OUT_CSV="$RESULTS_DIR/results.csv"

mkdir -p "$RESULTS_DIR"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

if [ ! -f "$EXEC_SCRIPT" ]; then
    echo "ERROR: exec_esp32.py not found at $EXEC_SCRIPT"
    exit 1
fi

echo ">>> Compiling scaling worker (.wasm + .aot)"
cd "$CONTAINER_DIR"
bash create_container.bash scaling_worker.c

if [ ! -f "$SCALING_DIR/scaling_worker.wasm" ]; then
    echo "ERROR: Missing $SCALING_DIR/scaling_worker.wasm"
    exit 1
fi

if [ ! -f "$SCALING_DIR/scaling_worker.aot" ]; then
    echo "ERROR: Missing $SCALING_DIR/scaling_worker.aot"
    echo "Ensure wamrc is available so both interpreter and AOT can be benchmarked."
    exit 1
fi

echo ">>> Building, flashing, and capturing scaling benchmark output"
cd "$SCRIPT_DIR"
python3 "$EXEC_SCRIPT" "$OUT_CSV" \
    idf.py -B build_scaling -DBUILD_SCALING_BENCHMARK=ON build flash monitor
