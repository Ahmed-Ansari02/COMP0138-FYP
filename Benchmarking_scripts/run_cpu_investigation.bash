#!/bin/bash
#
# run_cpu_investigation.bash
# Investigate elevated Core 1 CPU usage under WAMR vs Native.
#
# Hypothesis background:
#   Core 1 hosts reader_task + calculate_cpu_usage (identical code in both
#   variants), yet Core 1 utilisation is ~1.5x higher under WAMR than Native.
#   The WASM pthread is pinned to Core 0 by default, so the overhead is not
#   local execution — suspected causes: memory-bus contention, shared static
#   pressure, or misattributed ticks during cross-core mutex handoff.
#
# This script sweeps the two toggles in Wasm_controller/main/wasm.c that let
# us isolate those factors:
#   - CONTROL_ALGORITHM_CORE in {0, 1}
#       Core 0 -> WASM off Core 1 (baseline)
#       Core 1 -> WASM co-located with reader_task; if Core 1 CPU stays the
#                 same, overhead is systemic (bus/SMP), not pinning-induced.
#   - USE_AOT in {0, 1}
#       Interpreter vs AOT: narrows whether overhead tracks dispatch cost.
#
# Plus a native baseline (TEST_MODE=5, bang_bang_finite) for comparison.
#
# Each run writes results.csv into:
#   experiments/cpu_investigation/<timestamp>/<variant>/
#
# After all runs, a summary.csv is generated with mean/median Core 0 and
# Core 1 CPU utilisation per configuration.
#
# Usage:
#   bash run_cpu_investigation.bash [serial_port]
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

SERIAL_PORT="${1:-${ESPPORT:-}}"

# Fix test workload: bang_bang_finite (deterministic, 1000-iter, no hotswap noise)
TEST_MODE=5

if ! command -v idf.py >/dev/null 2>&1; then
  echo "ERROR: idf.py not found. Source ESP-IDF export.sh first."
  exit 1
fi
for f in "$NATIVE_MAIN" "$WAMR_MAIN" "$EXEC_SCRIPT"; do
  [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

TIMESTAMP="$(date +"%Y%m%d_%H%M%S")"
RESULTS_BASE="$ROOT_DIR/experiments/cpu_investigation/$TIMESTAMP"
mkdir -p "$RESULTS_BASE"

NATIVE_BACKUP="$(mktemp)"
WAMR_BACKUP="$(mktemp)"
cp "$NATIVE_MAIN" "$NATIVE_BACKUP"
cp "$WAMR_MAIN" "$WAMR_BACKUP"

restore_sources() {
  cp "$NATIVE_BACKUP" "$NATIVE_MAIN"
  cp "$WAMR_BACKUP" "$WAMR_MAIN"
}

free_serial_port() {
  [ -z "${SERIAL_PORT:-}" ] && return 0
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
  local file="$1" name="$2" value="$3"
  sed -i.bak -E "s|^#define[[:space:]]+${name}[[:space:]]+[^/]*|#define ${name} ${value} |" "$file"
  rm -f "${file}.bak"
}

run_capture() {
  local out_csv="$1" workdir="$2" use_aot="${3:-}"
  (
    cd "$workdir"
    local -a idf_cmd=(idf.py)
    [ -n "$use_aot" ] && idf_cmd+=(-D "USE_AOT=${use_aot}")
    [ -n "$SERIAL_PORT" ] && idf_cmd+=(-p "$SERIAL_PORT")
    python3 "$EXEC_SCRIPT" "$out_csv" "${idf_cmd[@]}" build flash monitor
  )
}

# -----------------------------------------------------------------------
# 1. Native baseline
# -----------------------------------------------------------------------
echo ""
echo "===================================================================="
echo "[1/5] Native baseline (TEST_MODE=$TEST_MODE)"
echo "===================================================================="
patch_define "$NATIVE_MAIN" "TEST_MODE" "$TEST_MODE"
out_dir="$RESULTS_BASE/native_baseline"
mkdir -p "$out_dir"
free_serial_port
run_capture "$out_dir/results.csv" "$NATIVE_DIR"
free_serial_port
sleep 2

# -----------------------------------------------------------------------
# 2-5. WAMR variants. Rebuild containers once.
# -----------------------------------------------------------------------
echo ""
echo "===================================================================="
echo "Rebuilding WASM/AOT containers"
echo "===================================================================="
(cd "$WAMR_CONTAINER_DIR" && bash create_container.bash)

patch_define "$WAMR_MAIN" "TEST_MODE" "$TEST_MODE"

run_wamr_variant() {
  local label="$1" core="$2" use_aot="$3"
  echo ""
  echo "===================================================================="
  echo "WAMR variant: $label (CONTROL_ALGORITHM_CORE=$core, USE_AOT=$use_aot)"
  echo "===================================================================="
  patch_define "$WAMR_MAIN" "CONTROL_ALGORITHM_CORE" "$core"
  patch_define "$WAMR_MAIN" "USE_AOT" "$use_aot"

  local exec_mode
  [ "$use_aot" -eq 1 ] && exec_mode="ON" || exec_mode="OFF"

  local out_dir="$RESULTS_BASE/$label"
  mkdir -p "$out_dir"
  free_serial_port
  run_capture "$out_dir/results.csv" "$WAMR_DIR" "$exec_mode"
  free_serial_port
  sleep 2
}

echo ""
echo "[2/5] WAMR interpreter, WASM on Core 0 (default pinning)"
run_wamr_variant "wamr_interp_core0" 0 0

echo ""
echo "[3/5] WAMR interpreter, WASM on Core 1 (co-located with reader_task)"
run_wamr_variant "wamr_interp_core1" 1 0

echo ""
echo "[4/5] WAMR AOT, WASM on Core 0"
run_wamr_variant "wamr_aot_core0" 0 1

echo ""
echo "[5/5] WAMR AOT, WASM on Core 1"
run_wamr_variant "wamr_aot_core1" 1 1

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------
echo ""
echo "===================================================================="
echo "Computing summary"
echo "===================================================================="

SUMMARY_CSV="$RESULTS_BASE/summary.csv"
python3 - "$RESULTS_BASE" "$SUMMARY_CSV" <<'PY'
import csv, os, statistics, sys
base, out = sys.argv[1], sys.argv[2]

def stats(path):
    c0, c1, ov = [], [], []
    if not os.path.exists(path):
        return None
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            for key, bucket in (("cpu_core0", c0), ("cpu_core1", c1), ("cpu_overall", ov)):
                v = row.get(key, "").strip()
                if v:
                    try: bucket.append(float(v))
                    except ValueError: pass
    if not c0:
        return None
    return {
        "core0_mean":   statistics.mean(c0),
        "core0_median": statistics.median(c0),
        "core1_mean":   statistics.mean(c1),
        "core1_median": statistics.median(c1),
        "overall_mean": statistics.mean(ov),
        "samples":      len(c0),
    }

rows = []
for variant in sorted(os.listdir(base)):
    p = os.path.join(base, variant, "results.csv")
    s = stats(p)
    if s is None:
        continue
    rows.append((variant, s))

with open(out, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["variant", "samples", "core0_mean", "core0_median",
                "core1_mean", "core1_median", "overall_mean"])
    for v, s in rows:
        w.writerow([v, s["samples"],
                    f"{s['core0_mean']:.2f}", f"{s['core0_median']:.2f}",
                    f"{s['core1_mean']:.2f}", f"{s['core1_median']:.2f}",
                    f"{s['overall_mean']:.2f}"])

print(f"\nSummary -> {out}\n")
print(f"{'variant':<25} {'core0_mean':>10} {'core1_mean':>10} {'overall':>10}")
print("-" * 60)
for v, s in rows:
    print(f"{v:<25} {s['core0_mean']:>10.2f} {s['core1_mean']:>10.2f} {s['overall_mean']:>10.2f}")
PY

echo ""
echo "Done. Results: $RESULTS_BASE"
