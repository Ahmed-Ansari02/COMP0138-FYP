#!/bin/bash
#
# run_all_benchmarks.bash — Automates the full IPC benchmark sweep.
# For each payload size and IPC method, it:
#   1. Recompiles WASM containers with the payload size
#   2. Patches IPC_METHOD and PAYLOAD_SIZE in the source
#   3. Builds, flashes, and captures results via exec_esp32.py
#
# Results are saved to: results/<method_name>/payload_<N>/
#
# Usage:
#   bash run_all_benchmarks.bash           # run all methods
#   bash run_all_benchmarks.bash 5         # run only method 5
#   bash run_all_benchmarks.bash 2 3       # run methods 2 and 3
#
# Requires: ESP-IDF environment, wasi-sdk at /opt/wasi-sdk

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MAIN_C="$SCRIPT_DIR/main/ipc_benchmark.c"
SHARED_H="$SCRIPT_DIR/main/shared_data.h"
CONTAINER_DIR="$SCRIPT_DIR/containers"
RESULTS_BASE="$ROOT_DIR/experiments/ipc_benchmark"
EXEC_SCRIPT="$SCRIPT_DIR/../exec_esp32.py"

# ---- CONFIGURATION ----
# Payload sizes to test (bytes added to base 12-byte SensorMessage)
PAYLOAD_SIZES=(0 52 244)

# All available methods: "define_value:name"
ALL_METHODS=(
    "0:shared_memory"
    "2:stream_buffer"
    "3:queue"
    "4:bridge"
    "5:native_to_wasm"
)

# ---- FILTER METHODS BY ARGUMENTS ----
if [ $# -gt 0 ]; then
    METHODS=()
    for arg in "$@"; do
        for entry in "${ALL_METHODS[@]}"; do
            IFS=':' read -r id name <<< "$entry"
            if [ "$id" = "$arg" ]; then
                METHODS+=("$entry")
            fi
        done
    done
    if [ ${#METHODS[@]} -eq 0 ]; then
        echo "ERROR: No valid methods found for arguments: $@"
        echo "Valid method IDs: 0 (shared_memory), 2 (stream_buffer), 3 (queue), 4 (bridge), 5 (native_to_wasm)"
        exit 1
    fi
else
    METHODS=("${ALL_METHODS[@]}")
fi

echo "Methods to run:"
for entry in "${METHODS[@]}"; do
    IFS=':' read -r id name <<< "$entry"
    echo "  $id: $name"
done
echo ""

# ---- HELPERS ----

patch_define() {
    # Usage: patch_define <file> <define_name> <new_value>
    local file="$1" name="$2" value="$3"
    sed -i.bak "s/^#define ${name} .*/#define ${name} ${value}/" "$file"
    rm -f "${file}.bak"
}

log() {
    echo ""
    echo "========================================================================"
    echo "  $1"
    echo "========================================================================"
    echo ""
}

# ---- PREFLIGHT CHECKS ----
if ! command -v idf.py &>/dev/null; then
    echo "ERROR: idf.py not found. Source ESP-IDF export.sh first."
    exit 1
fi

if [ ! -f "$EXEC_SCRIPT" ]; then
    echo "ERROR: exec_esp32.py not found at $EXEC_SCRIPT"
    exit 1
fi

# ---- MAIN LOOP ----

for payload in "${PAYLOAD_SIZES[@]}"; do
    total_size=$((12 + payload))
    log "PAYLOAD_SIZE=${payload} (total struct = ${total_size} bytes)"

    # Step 1: Recompile WASM containers with this payload size
    echo ">>> Compiling WASM containers (PAYLOAD_SIZE=${payload})..."
    cd "$CONTAINER_DIR"
    PAYLOAD_SIZE=$payload bash create_container.bash
    cd "$SCRIPT_DIR"

    for method_entry in "${METHODS[@]}"; do
        IFS=':' read -r method_id method_name <<< "$method_entry"

        log "Method ${method_id} (${method_name}) | PAYLOAD_SIZE=${payload}"

        # Step 2: Patch source defines
        patch_define "$MAIN_C" "IPC_METHOD" "$method_id"
        patch_define "$MAIN_C" "PAYLOAD_SIZE" "$payload"

        # Step 3: Determine output directory
        out_dir="$RESULTS_BASE/${method_name}/payload_${payload}"
        mkdir -p "$out_dir"
        out_csv="$out_dir/results.csv"

        # Step 4: Build, flash, and capture
        echo ">>> Building and flashing..."
        cd "$SCRIPT_DIR"
        python3 "$EXEC_SCRIPT" "$out_csv" idf.py build flash monitor

        echo ">>> Results saved to: $out_dir/"
        echo ""
    done
done

# ---- RESTORE DEFAULTS ----
patch_define "$MAIN_C" "IPC_METHOD" "0"
patch_define "$MAIN_C" "PAYLOAD_SIZE" "0"

log "ALL BENCHMARKS COMPLETE"
echo "Results are in: $RESULTS_BASE/"
echo ""
echo "Directory structure:"
find "$RESULTS_BASE" -name "*.csv" | sort
