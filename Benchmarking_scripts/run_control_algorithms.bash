#!/bin/bash
#
# run_control_algorithms.bash
# Run control algorithms one-by-one, capturing CSV after each run:
#   1) bang_bang
#   2) pid
#   3) e2e
#
# Modes used:
#   Native_controller/main/native.c:
#     TEST_MODE 5=b
#     TEST_MODE 6=pid
#     TEST_MODE 0=e2e
#   Wasm_controller/main/wasm.c:
#     TEST_MODE 5=b
#     TEST_MODE 6=pid
#     TEST_MODE 0=e2e
#
# Usage:
#   bash run_control_algorithms.bash                # run native + wamr (interpreter)
#   bash run_control_algorithms.bash native         # run native only
#   bash run_control_algorithms.bash wamr           # run wamr only (interpreter)
#   bash run_control_algorithms.bash wamr aot       # run wamr only (AOT)
#   bash run_control_algorithms.bash all both       # run native + wamr (wasm + aot)
#   bash run_control_algorithms.bash all wasm /dev/cu.usbserial-XXXX
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

RUN_TARGET="${1:-all}"         # all|native|wamr
WAMR_EXEC_MODE="${2:-wasm}"    # wasm|aot|both
SERIAL_PORT="${3:-${ESPPORT:-}}"

case "$RUN_TARGET" in
  all|native|wamr) ;;
  *)
    echo "Usage: bash run_control_algorithms.bash [all|native|wamr] [wasm|aot|both] [serial_port]"
    exit 1
    ;;
esac

case "$WAMR_EXEC_MODE" in
  wasm|aot|both) ;;
  *)
    echo "Usage: bash run_control_algorithms.bash [all|native|wamr] [wasm|aot|both] [serial_port]"
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
RESULTS_BASE="$ROOT_DIR/experiments/control_algorithms/$TIMESTAMP"
mkdir -p "$RESULTS_BASE"

ALGO_NAMES=("bang_bang" "pid" "e2e")
ALGO_MODES=(5 6 0)

NATIVE_BACKUP="$(mktemp)"
WAMR_BACKUP="$(mktemp)"
cp "$NATIVE_MAIN" "$NATIVE_BACKUP"
cp "$WAMR_MAIN" "$WAMR_BACKUP"

free_serial_port() {
  if [ -z "${SERIAL_PORT:-}" ]; then
    return 0
  fi

  # Clear known monitor/flash processes that can keep a lock on the port.
  pkill -f "idf.py.*monitor" >/dev/null 2>&1 || true
  pkill -f "esptool.*${SERIAL_PORT}" >/dev/null 2>&1 || true
  pkill -f "miniterm.*${SERIAL_PORT}" >/dev/null 2>&1 || true

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

run_capture() {
  local out_csv="$1"
  local workdir="$2"
  local timeout_s="${3:-90}"
  local wamr_use_aot="${4:-}"

  (
    cd "$workdir"
    local -a idf_cmd=(idf.py)
    if [ -n "$wamr_use_aot" ]; then
      idf_cmd+=(-D "USE_AOT=${wamr_use_aot}")
    fi
    if [ -n "$SERIAL_PORT" ]; then
      idf_cmd+=(-p "$SERIAL_PORT")
      EXEC_ESP32_TIMEOUT_S="$timeout_s" python3 "$EXEC_SCRIPT" "$out_csv" "${idf_cmd[@]}" build flash monitor
    else
      EXEC_ESP32_TIMEOUT_S="$timeout_s" python3 "$EXEC_SCRIPT" "$out_csv" "${idf_cmd[@]}" build flash monitor
    fi
  )
}

run_native() {
  for i in "${!ALGO_NAMES[@]}"; do
    local algo="${ALGO_NAMES[$i]}"
    local mode="${ALGO_MODES[$i]}"

    echo ""
    echo "===================================================================="
    echo "Native: ${algo} (TEST_MODE=${mode})"
    echo "===================================================================="

    patch_define "$NATIVE_MAIN" "TEST_MODE" "$mode"

    local out_dir="$RESULTS_BASE/native/${algo}"
    local out_csv="$out_dir/results.csv"
    local timeout_s=130
    if [ "$mode" -eq 0 ]; then
      timeout_s=150
    fi
    mkdir -p "$out_dir"

    free_serial_port
    run_capture "$out_csv" "$NATIVE_DIR" "$timeout_s"
    free_serial_port
    sleep 2
  done
}

run_wamr_for_exec_mode() {
  local use_aot="$1" # 0 or 1
  local exec_name
  local cmake_use_aot
  if [ "$use_aot" -eq 1 ]; then
    exec_name="aot"
    cmake_use_aot="ON"
  else
    exec_name="wasm"
    cmake_use_aot="OFF"
  fi

  for i in "${!ALGO_NAMES[@]}"; do
    local algo="${ALGO_NAMES[$i]}"
    local mode="${ALGO_MODES[$i]}"

    echo ""
    echo "===================================================================="
    echo "WAMR (${exec_name}): ${algo} (TEST_MODE=${mode}, USE_AOT=${use_aot})"
    echo "===================================================================="

    patch_define "$WAMR_MAIN" "TEST_MODE" "$mode"
    patch_define "$WAMR_MAIN" "USE_AOT" "$use_aot"

    local out_dir="$RESULTS_BASE/wamr/${exec_name}/${algo}"
    local out_csv="$out_dir/results.csv"
    local timeout_s=130
    if [ "$mode" -eq 0 ]; then
      timeout_s=150
    fi
    mkdir -p "$out_dir"

    echo "[WAMR] CMake USE_AOT=${cmake_use_aot}"
    free_serial_port
    run_capture "$out_csv" "$WAMR_DIR" "$timeout_s" "$cmake_use_aot"
    free_serial_port
    sleep 2
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

  case "$WAMR_EXEC_MODE" in
    wasm) run_wamr_for_exec_mode 0 ;;
    aot) run_wamr_for_exec_mode 1 ;;
    both)
      run_wamr_for_exec_mode 0
      run_wamr_for_exec_mode 1
      ;;
  esac
}

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
