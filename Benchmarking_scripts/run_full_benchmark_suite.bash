#!/bin/bash
#
# run_full_benchmark_suite.bash
# Run the full benchmark suite across:
#   - Native_controller
#   - Wasm_controller (interpreter + AOT)
#
# Matrix:
#   Native:  7 test modes x 2 cores = 14 runs
#   WAMR:    7 test modes x 2 cores x 2 exec modes = 28 runs
#   Total:   42 flashes / runs
#
# Usage:
#   bash run_full_benchmark_suite.bash
#   bash run_full_benchmark_suite.bash native
#   bash run_full_benchmark_suite.bash wamr
#   bash run_full_benchmark_suite.bash all /dev/cu.usbserial-XXXX
#
# Prereq: source the ESP-IDF export.sh script so idf.py is available.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
NATIVE_DIR="$ROOT_DIR/Native_controller"
WAMR_DIR="$ROOT_DIR/Wasm_controller"
NATIVE_MAIN="$NATIVE_DIR/main/native.c"
WAMR_MAIN="$WAMR_DIR/main/wasm.c"
WAMR_CONTAINER_DIR="$WAMR_DIR/containers"
EXEC_SCRIPT="$ROOT_DIR/exec_esp32.py"

RUN_TARGET="${1:-all}"          # all|native|wamr
SERIAL_PORT="${2:-${ESPPORT:-}}"

case "$RUN_TARGET" in
  all|native|wamr) ;;
  *)
    echo "Usage: bash run_full_benchmark_suite.bash [all|native|wamr] [serial_port]"
    exit 1
    ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: idf.py not found. Source ESP-IDF export.sh first."
  exit 1
fi

if [ ! -f "$EXEC_SCRIPT" ]; then
  echo "ERROR: exec_esp32.py not found at $EXEC_SCRIPT"
  exit 1
fi

if [ ! -f "$NATIVE_MAIN" ] || [ ! -f "$WAMR_MAIN" ]; then
  echo "ERROR: Expected source files not found."
  exit 1
fi

TIMESTAMP="$(date +"%Y%m%d_%H%M%S")"
RESULTS_BASE="$ROOT_DIR/experiments/full_benchmark_suite/$TIMESTAMP"
mkdir -p "$RESULTS_BASE"

TEST_NAMES=(
  "bang_bang"
  "pid"
  "e2e"
  "hotswap"
  "fault_null_ptr"
  "fault_overflow"
  "fault_infinite_loop"
)
TEST_MODES=(5 6 0 1 2 3 4)
CONTROL_CORES=(0 1)
WAMR_EXEC_NAMES=("wasm" "aot")
WAMR_EXEC_VALUES=(0 1)

NATIVE_BACKUP="$(mktemp)"
WAMR_BACKUP="$(mktemp)"
cp "$NATIVE_MAIN" "$NATIVE_BACKUP"
cp "$WAMR_MAIN" "$WAMR_BACKUP"

free_serial_port() {
  if [ -z "${SERIAL_PORT:-}" ]; then
    return 0
  fi

  pkill -f "idf.py.*monitor" >/dev/null 2>&1 || true
  pkill -f "esptool.*${SERIAL_PORT}" >/dev/null 2>&1 || true
  pkill -f "miniterm.*${SERIAL_PORT}" >/dev/null 2>&1 || true
  pkill -f "idf_monitor.py.*${SERIAL_PORT}" >/dev/null 2>&1 || true

  if command -v lsof >/dev/null 2>&1; then
    local pids
    pids="$(lsof -t "$SERIAL_PORT" 2>/dev/null | sort -u || true)"
    if [ -n "$pids" ]; then
      echo "$pids" | xargs kill >/dev/null 2>&1 || true
      sleep 1
      pids="$(lsof -t "$SERIAL_PORT" 2>/dev/null | sort -u || true)"
      if [ -n "$pids" ]; then
        echo "$pids" | xargs kill -9 >/dev/null 2>&1 || true
      fi
    fi
  fi
}

restore_sources() {
  cp "$NATIVE_BACKUP" "$NATIVE_MAIN"
  cp "$WAMR_BACKUP" "$WAMR_MAIN"
}

cleanup_on_exit() {
  restore_sources
  free_serial_port
  rm -f "$NATIVE_BACKUP" "$WAMR_BACKUP"
}
trap cleanup_on_exit EXIT

patch_define() {
  local file="$1"
  local name="$2"
  local value="$3"
  sed -i.bak -E "s/^#define[[:space:]]+${name}[[:space:]]+.*/#define ${name} ${value}/" "$file"
  rm -f "${file}.bak"
}

is_fault_mode() {
  local mode="$1"
  [ "$mode" -ge 2 ] && [ "$mode" -le 4 ]
}

timeout_for_mode() {
  local mode="$1"
  case "$mode" in
    0) echo 150 ;;  # e2e
    1) echo 180 ;;  # hotswap
    2|3|4) echo 90 ;;  # fault tests
    5|6) echo 130 ;;  # finite control runs
    *) echo 90 ;;
  esac
}

count_runs() {
  local total=0
  if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "native" ]; then
    total=$((total + ${#TEST_MODES[@]} * ${#CONTROL_CORES[@]}))
  fi
  if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "wamr" ]; then
    total=$((total + ${#TEST_MODES[@]} * ${#CONTROL_CORES[@]} * ${#WAMR_EXEC_VALUES[@]}))
  fi
  echo "$total"
}

run_capture() {
  local out_csv="$1"
  local workdir="$2"
  local timeout_s="$3"
  local raw_log="$4"
  local wamr_use_aot="${5:-}"

  mkdir -p "$(dirname "$out_csv")"
  (
    cd "$workdir"
    local -a idf_cmd=(idf.py)
    if [ -n "$wamr_use_aot" ]; then
      idf_cmd+=(-D "USE_AOT=${wamr_use_aot}")
    fi
    if [ -n "$SERIAL_PORT" ]; then
      idf_cmd+=(-p "$SERIAL_PORT")
    fi

    EXEC_ESP32_TIMEOUT_S="$timeout_s" \
      python3 "$EXEC_SCRIPT" "$out_csv" "${idf_cmd[@]}" build flash monitor 2>&1 | tee "$raw_log"
  )
}

run_case() {
  local mode="$1"
  shift

  if is_fault_mode "$mode"; then
    if ! run_capture "$@"; then
      echo "[INFO] Expected non-zero exit for fault mode ${mode}; continuing."
    fi
  else
    run_capture "$@"
  fi
}

run_native() {
  local test_name
  local mode
  local core
  local out_dir
  local out_csv
  local raw_log
  local timeout_s

  for i in "${!TEST_NAMES[@]}"; do
    test_name="${TEST_NAMES[$i]}"
    mode="${TEST_MODES[$i]}"

    for core in "${CONTROL_CORES[@]}"; do
      echo ""
      echo "===================================================================="
      echo "Native: ${test_name} (TEST_MODE=${mode}, CONTROL_ALGORITHM_CORE=${core})"
      echo "===================================================================="

      patch_define "$NATIVE_MAIN" "TEST_MODE" "$mode"
      patch_define "$NATIVE_MAIN" "CONTROL_ALGORITHM_CORE" "$core"

      out_dir="$RESULTS_BASE/native/core${core}/${test_name}"
      out_csv="$out_dir/results.csv"
      raw_log="$out_dir/raw_output.log"
      timeout_s="$(timeout_for_mode "$mode")"

      free_serial_port
      run_case "$mode" "$out_csv" "$NATIVE_DIR" "$timeout_s" "$raw_log"
      free_serial_port
      sleep 2
    done
  done
}

run_wamr_for_exec_mode() {
  local exec_name="$1"
  local use_aot="$2"
  local cmake_use_aot="OFF"
  local test_name
  local mode
  local core
  local out_dir
  local out_csv
  local raw_log
  local timeout_s

  if [ "$use_aot" -eq 1 ]; then
    cmake_use_aot="ON"
  fi

  for i in "${!TEST_NAMES[@]}"; do
    test_name="${TEST_NAMES[$i]}"
    mode="${TEST_MODES[$i]}"

    for core in "${CONTROL_CORES[@]}"; do
      echo ""
      echo "===================================================================="
      echo "WAMR (${exec_name}): ${test_name} (TEST_MODE=${mode}, USE_AOT=${use_aot}, CONTROL_ALGORITHM_CORE=${core})"
      echo "===================================================================="

      patch_define "$WAMR_MAIN" "TEST_MODE" "$mode"
      patch_define "$WAMR_MAIN" "USE_AOT" "$use_aot"
      patch_define "$WAMR_MAIN" "CONTROL_ALGORITHM_CORE" "$core"

      out_dir="$RESULTS_BASE/wamr/${exec_name}/core${core}/${test_name}"
      out_csv="$out_dir/results.csv"
      raw_log="$out_dir/raw_output.log"
      timeout_s="$(timeout_for_mode "$mode")"

      echo "[WAMR] CMake USE_AOT=${cmake_use_aot}"
      free_serial_port
      run_case "$mode" "$out_csv" "$WAMR_DIR" "$timeout_s" "$raw_log" "$cmake_use_aot"
      free_serial_port
      sleep 2
    done
  done
}

run_wamr() {
  echo ""
  echo "===================================================================="
  echo "WAMR: Rebuilding containers (.wasm + .aot)"
  echo "===================================================================="
  (
    cd "$WAMR_CONTAINER_DIR"
    bash create_container.bash
  )

  local i
  for i in "${!WAMR_EXEC_NAMES[@]}"; do
    run_wamr_for_exec_mode "${WAMR_EXEC_NAMES[$i]}" "${WAMR_EXEC_VALUES[$i]}"
  done
}

TOTAL_RUNS="$(count_runs)"
echo "Output directory: $RESULTS_BASE"
echo "Planned runs:"
if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "native" ]; then
  echo "  Native: 14"
fi
if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "wamr" ]; then
  echo "  WAMR:   28"
fi
echo "  Total:  ${TOTAL_RUNS}"
echo ""

if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "native" ]; then
  run_native
fi

if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "wamr" ]; then
  run_wamr
fi

if [ -n "$SERIAL_PORT" ]; then
  echo "Using serial port: $SERIAL_PORT"
fi

echo ""
echo "Done. Results saved under:"
echo "  $RESULTS_BASE"
echo ""
