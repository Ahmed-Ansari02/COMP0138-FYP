# Phase 2/3 Experiments: Hot Swap, Fault Tolerance, and Containerization

## Experiment Context

This document extends the Phase 1 benchmark write-up with Phase 2/3 experiments focused on:

1. Runtime hot swapping of control logic
2. Fault containment and recovery behavior
3. Containerization strategy overhead via IPC microbenchmarks

All values below are taken from captured CSV/log artifacts in this repository (no synthetic values).

## A. Hot-Swap Evaluation

### A.1 Test Intent

Goal: quantify the runtime cost of switching control algorithms and verify continuity of control-state observables (temperature around swap point).

### A.2 Data Sources

- Native:
  - `Native_controller/results/Phase2/Hot_swap/native_hotswap.csv`
  - `Native_controller/results/Phase2/Hot_swap/native.csv`
  - `Native_controller/results/Phase2/Hot_swap/native_runtime_stats.csv`
- WAMR:
  - `Wasm_controller/results/Phase2/Hot_swap/wamr_hotswap.csv`
  - `Wasm_controller/results/Phase2/Hot_swap/wamr.csv`
  - `Wasm_controller/results/Phase2/Hot_swap/wamr_runtime_stats.csv`

### A.3 Raw Swap Measurements

| Platform | swap_index | swap_latency_us | load_time_us | instantiate_time_us | temp_at_swap | temp_after_swap | swap_timestamp_us |
|---|---:|---:|---:|---:|---:|---:|---:|
| Native | 0 | 8 | N/A | N/A | 33.88 | 33.88 | 30,065,668 |
| WAMR | 0 | 86,573 | 55,398 | 30,095,573 | 36.27 | 36.48 | 30,272,631 |

### A.4 Derived Comparison

| Comparison | Value |
|---|---:|
| WAMR/native swap-latency ratio | 10,821.6x |
| WAMR temp delta across swap | +0.21 C |
| Native temp delta across swap | +0.00 C |

### A.5 Runtime-Level Observations

`*_runtime_stats.csv` indicates a clear difference in task profile during hot-swap runs:

- Native run: `CPU Usage` task reported at ~6%.
- WAMR run: `CPU Usage` task reported at ~15%; `WASM Runtime` task visible as a separate contributor.

This aligns with expected extra work for module management and runtime mediation in WAMR.

### A.6 Interpretation

1. Native hot-swap is near-instant at microsecond scale.
2. WAMR hot-swap is operationally successful but incurs significant latency overhead relative to native.
3. Despite added latency, both paths preserve thermal continuity around the swap event (no abrupt temperature discontinuity).

## B. Fault-Tolerance Evaluation

### B.1 Test Intent

Goal: compare fault impact in unsandboxed native execution vs sandboxed WAMR execution, including:

- Detection behavior
- Recovery behavior
- Host survivability indicators (`adc_alive`, post-recovery heap/temperature)

### B.2 Fault Classes

Three fault classes were exercised:

1. Null pointer access
2. Stack overflow / memory corruption path
3. Infinite loop (watchdog path)

### B.3 Data Sources

- Native fault artifacts:
  - `Native_controller/results/Phase2/Fault_test/Null_ptr/native_fault_native.csv`
  - `Native_controller/results/Phase2/Fault_test/Stack_overflow/native_fault_native.csv`
  - `Native_controller/results/Phase2/Fault_test/Infinite_loop/native_fault_native.csv`
  - `Native_controller/results/Phase2/Fault_test/Null_ptr/native_crash.log`
  - `Native_controller/results/Phase2/Fault_test/Stack_overflow/native_crash.log`
  - `Native_controller/results/Phase2/Fault_test/Infinite_loop/native_wdt.log`
- WAMR fault artifacts:
  - `Wasm_controller/results/Phase2/Fault_test/Null_ptr/Null_ptr_fault.csv`
  - `Wasm_controller/results/Phase2/Fault_test/Stack_overflow/wamr_overflow_fault.csv`
  - `Wasm_controller/results/Phase2/Fault_test/Infinite_loop/infinite_loop_fault.csv`
  - `Wasm_controller/results/Phase2/Fault_test/*/*_runtime_stats.csv`

### B.4 Native Failure Signatures

Observed native runtime signatures:

- Null pointer: `Guru Meditation Error` with `StoreProhibited`.
- Stack overflow/corruption scenario: `Guru Meditation Error` with `LoadProhibited`.
- Infinite loop: repeated task watchdog events indicating `IDLE0 (CPU0)` starvation while control task occupies CPU0.

Pre-fault markers (captured before crash/lockup):

| Fault | pre_fault_iterations | temp_at_fault | heap_at_fault | timestamp_us |
|---|---:|---:|---:|---:|
| null_ptr | 10 | 38.03 | 253,984 | 1,079,397 |
| stack_overflow | 10 | 36.21 | 253,984 | 1,076,890 |
| infinite_loop | 10 | 38.70 | 253,984 | 1,076,936 |

### B.5 WAMR Fault and Recovery Metrics

| Fault | fault_type | detection_latency_us | recovery_latency_us | temp_at_fault | temp_after_recovery | heap_at_fault | heap_after_recovery | adc_alive | fault_description |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| null_ptr | 1 | 1,097,632 | 3,643,534 | 35.70 | 37.21 | 209,800 | 209,580 | 1 | wasm_exception |
| stack_overflow | 2 | 6,972,042 | 3,140,993 | 33.76 | 37.42 | 209,800 | 209,580 | 1 | watchdog_terminated |
| infinite_loop | 2 | 6,974,759 | 3,151,165 | 33.67 | 37.52 | 209,800 | 209,580 | 1 | watchdog_terminated |

### B.6 Derived Summary (WAMR)

| Aggregate | Value |
|---|---:|
| Mean detection latency (all 3 faults) | 5,014,811 us |
| Mean recovery latency (all 3 faults) | 3,311,897 us |
| Mean detection latency (watchdog faults only) | 6,973,401 us |
| Mean recovery latency (watchdog faults only) | 3,146,079 us |
| Post-recovery `adc_alive` success rate | 3/3 |

### B.7 Interpretation

1. Native failures propagate to system-level panic/watchdog behavior; there is no in-band recovery path in these tests.
2. WAMR faults remain classifiable and recoverable via the instrumented fault-handling path.
3. Recovery is not instantaneous (millisecond-to-second scale), but host integrity indicators remain valid after recovery (`adc_alive=1`, stable heap level).

## C. Containerization Strategy Evaluation (IPC Benchmark)

### C.1 Test Intent

Goal: measure overhead tradeoffs across inter-container communication methods that represent different containerization approaches.

### C.2 Measurement Basis

- Benchmark root: `ipc_benchmark/results/`
- Run mode used for cross-method comparison: `same_core`, 10,000 iterations.
- Reference payload for primary table: `payload_52`.
- Conversion: ESP32 at 240 MHz, so `1 cycle ~= 4.17 ns`, `microseconds ~= cycles / 240`.

### C.3 Payload-52 Comparison (Primary)

| Method | mean_cycles | approx_mean_us | p50_cycles | p95_cycles | p99_cycles | free_heap_start | free_heap_end |
|---|---:|---:|---:|---:|---:|---:|---:|
| shared_memory | 25.2 | 0.105 | 25 | 25 | 25 | 104,464 | 104,464 |
| host_bridge | 16,974.8 | 70.728 | 16,717 | 17,951 | 18,645 | 63,448 | 63,448 |
| queue | 49,241.1 | 205.171 | 19,433 | 288,318 | 428,928 | 63,164 | 63,104 |
| stream_buffer | 59,612.1 | 248.384 | 20,846 | 345,699 | 416,795 | 63,276 | 63,216 |
| native_to_wasm | 419,763.4 | 1,749.014 | 419,778 | 420,839 | 421,026 | 42,256 | 42,256 |

### C.4 Relative Cost vs Shared Memory (Payload 52)

| Method | mean ratio vs shared_memory |
|---|---:|
| shared_memory | 1.0x |
| host_bridge | 673.6x |
| queue | 1,954.0x |
| stream_buffer | 2,365.6x |
| native_to_wasm | 16,657.3x |

### C.5 Payload Sensitivity Snapshot

Mean cycles stayed broadly stable between payloads 0, 52, and 244 for most methods:

| Payload | shared_memory | host_bridge | queue | stream_buffer | native_to_wasm |
|---|---:|---:|---:|---:|---:|
| 0 | 25.2 | 17,047.4 | 49,580.4 | 58,487.4 | 419,763.3 |
| 52 | 25.2 | 16,974.8 | 49,241.1 | 59,612.1 | 419,763.4 |
| 244 | 25.3 | 17,013.1 | 43,512.1 | 60,094.1 | 419,171.1 |

Interpretation: in this benchmark implementation, control-path/runtime overhead dominates payload-copy scaling for these payload sizes.

### C.6 Tail-Latency Characteristics

- `shared_memory` and `native_to_wasm` are tightly bounded (`p95`/`p99` near `p50`).
- `queue` and `stream_buffer` show substantial tail amplification (`p95` and `p99` far above `p50`), consistent with scheduling/queueing effects.
- `host_bridge` is low-jitter relative to RTOS queue-based methods.

### C.7 Interpretation

1. Shared memory is the latency baseline and best candidate where direct-memory sharing is acceptable.
2. Host bridge offers a middle ground with moderate latency and low spread.
3. Queue and stream buffer introduce significant latency variance that must be budgeted for real-time control loops.
4. Native-to-WASM transfer path is by far the highest-cost mechanism in these results.

## D. Threats to Validity and Limitations

1. Hot-swap CSVs currently contain one recorded swap event per run (`swap_index=0`), so variance across repeated swaps is not characterized here.
2. Fault evaluation covers one captured event per fault class; confidence in latency distributions would improve with repeated trials.
3. IPC results used here are same-core mode snapshots; cross-core behavior should be analyzed separately before making architecture-level claims.
4. Payload scaling conclusions apply only to tested sizes (0, 52, 244) and current implementation details.

## E. Reproducibility References

- Hot swap:
  - `Native_controller/results/Phase2/Hot_swap/`
  - `Wasm_controller/results/Phase2/Hot_swap/`
- Fault tolerance:
  - `Native_controller/results/Phase2/Fault_test/`
  - `Wasm_controller/results/Phase2/Fault_test/`
- Containerization IPC:
  - `ipc_benchmark/results/`
