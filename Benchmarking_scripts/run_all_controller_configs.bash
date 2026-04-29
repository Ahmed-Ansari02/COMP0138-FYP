#!/bin/bash
#
# run_all_controller_configs.bash
# Sweep all controller benchmark configurations for:
#   - Native_controller/main/native.c
#   - Wasm_controller/main/wasm.c
#
# Config matrix:
#   Native: TEST_MODE in {0,1,2,3,4}
#   WAMR:   TEST_MODE in {0,1,2,3,4} x USE_AOT in {0,1}
#
# Results are captured via ../exec_esp32.py into:
#   experiments/controller_config_sweep/<timestamp>/
#
# Usage:
#   bash run_all_controller_configs.bash                     # run native + wamr
#   bash run_all_controller_configs.bash native              # run only native
#   bash run_all_controller_configs.bash wamr                # run only wamr
#   bash run_all_controller_configs.bash all /dev/cu.usbserial-XXXX
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

RUN_TARGET="${1:-all}" # all|native|wamr
SERIAL_PORT="${2:-${ESPPORT:-}}"

case "$RUN_TARGET" in
  all|native|wamr) ;;
  *)
    echo "Usage: bash run_all_controller_configs.bash [all|native|wamr] [serial_port]"
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
RESULTS_BASE="$ROOT_DIR/experiments/controller_config_sweep/$TIMESTAMP"
mkdir -p "$RESULTS_BASE"

NATIVE_MODES=(0 1 2 3 4)
WAMR_MODES=(0 1 2 3 4)
WAMR_AOT_MODES=(0 1)

mode_name() {
  local mode="$1"
  case "$mode" in
    0) echo "e2e" ;;
    1) echo "hotswap" ;;
    2) echo "fault_null_ptr" ;;
    3) echo "fault_overflow" ;;
    4) echo "fault_infinite_loop" ;;
    5) echo "bang_bang" ;;
    6) echo "pid" ;;
    *) echo "mode_${mode}" ;;
  esac
}

NATIVE_BACKUP="$(mktemp)"
WAMR_BACKUP="$(mktemp)"
cp "$NATIVE_MAIN" "$NATIVE_BACKUP"
cp "$WAMR_MAIN" "$WAMR_BACKUP"

restore_sources() {
  cp "$NATIVE_BACKUP" "$NATIVE_MAIN"
  cp "$WAMR_BACKUP" "$WAMR_MAIN"
}

free_serial_port() {
  if [ -z "${SERIAL_PORT:-}" ]; then
    return 0
  fi

  pkill -f "idf.py.*monitor" >/dev/null 2>&1 || true
  pkill -f "idf_monitor.py.*${SERIAL_PORT}" >/dev/null 2>&1 || true
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

cleanup_on_exit() {
  restore_sources
  free_serial_port
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

run_capture() {
  local out_csv="$1"
  local workdir="$2"
  local wamr_use_aot="${3:-}"

  (
    cd "$workdir"
    local -a idf_cmd=(idf.py)
    if [ -n "$wamr_use_aot" ]; then
      idf_cmd+=(-D "USE_AOT=${wamr_use_aot}")
    fi
    if [ -n "$SERIAL_PORT" ]; then
      idf_cmd+=(-p "$SERIAL_PORT")
    fi
    python3 "$EXEC_SCRIPT" "$out_csv" "${idf_cmd[@]}" build flash monitor
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
  for mode in "${NATIVE_MODES[@]}"; do
    local test_name
    test_name="$(mode_name "$mode")"

    echo ""
    echo "===================================================================="
    echo "Native: TEST_MODE=$mode"
    echo "===================================================================="

    patch_define "$NATIVE_MAIN" "TEST_MODE" "$mode"

    local out_dir="$RESULTS_BASE/native/${test_name}"
    local out_csv="$out_dir/results.csv"
    mkdir -p "$out_dir"

    free_serial_port
    run_case "$mode" "$out_csv" "$NATIVE_DIR"
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

  for mode in "${WAMR_MODES[@]}"; do
    local test_name
    test_name="$(mode_name "$mode")"

    for use_aot in "${WAMR_AOT_MODES[@]}"; do
      echo ""
      echo "===================================================================="
      echo "WAMR: TEST_MODE=$mode USE_AOT=$use_aot"
      echo "===================================================================="

      patch_define "$WAMR_MAIN" "TEST_MODE" "$mode"
      patch_define "$WAMR_MAIN" "USE_AOT" "$use_aot"

      local exec_mode
      if [ "$use_aot" -eq 1 ]; then
        exec_mode="aot"
      else
        exec_mode="wasm"
      fi

      local out_dir="$RESULTS_BASE/wamr/${test_name}/${exec_mode}"
      local out_csv="$out_dir/results.csv"
      mkdir -p "$out_dir"

      free_serial_port
      if [ "$use_aot" -eq 1 ]; then
        run_case "$mode" "$out_csv" "$WAMR_DIR" "ON"
      else
        run_case "$mode" "$out_csv" "$WAMR_DIR" "OFF"
      fi
      free_serial_port
      sleep 2
    done
  done
}

if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "native" ]; then
  run_native
fi

if [ "$RUN_TARGET" = "all" ] || [ "$RUN_TARGET" = "wamr" ]; then
  run_wamr
fi

echo ""
echo "Done. Results saved under:"
echo "  $RESULTS_BASE"
if [ -n "$SERIAL_PORT" ]; then
  echo "Using serial port: $SERIAL_PORT"
fi
echo ""
