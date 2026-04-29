# Benchmark Results: Step-Response E2E Algorithm — WAMR vs Native

## Test Configuration

- **Algorithm**: `algorithm_e2e_step_response` — dedicated step-response test issuing clean heater ON/OFF steps from stable thermal baselines, measuring true round-trip signal propagation latency
- **Loop rate**: 100Hz (10ms `PERIOD_MS` per sampling iteration)
- **Structure**: 5s stabilisation at ambient + 6 step-response cycles (3s heating + 12s cooldown each) = **95s total runtime**
- **E2E samples produced**: 6 (one per heater ON step)
- **Circular buffer depth**: `NO_VALUES_TO_SAVE = 500` (captures last 500 loop/ADC/CPU samples)
- **Simulator**: Separate ESP32 running thermal physics simulation at 20Hz (50ms cycle), connected via DAC/ADC analog signals
- **Metrics collection**: `RECORD_METRICS = 1`, `CPU_MEASUREMENT_INTERVAL_MS = 10ms`
- **E2E threshold**: `E2E_THRESHOLD = 0.5`°C (reduced from previous 2.0°C)
- **CSV columns**: `index, exec_time_us, loop_time_us, adc_time_us, heater_time_us, temp_get_time_us, e2e_time_us, free_heap, cpu_core0, cpu_core1, cpu_overall, temp_mutex_wait, metrics_mutex_wait`

### Task Layout (both controllers)

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| Control algorithm / WASM Runtime | 0 | 5 | 4KB / 24KB | Executes step-response algorithm |
| ADC Reader | 1 | 5 | 4KB | Reads ADC + calibration at ~1kHz, writes `current_temp` under `temp_mutex`, detects E2E threshold crossing |
| CPU Usage | 1 | 3 | 4KB | Calls `uxTaskGetSystemState()` every 10ms, computes per-core idle deltas |
| IDLE0 / IDLE1 | 0 / 1 | 0 | — | FreeRTOS idle tasks (runtime used to derive CPU %) |

## E2E Measurement Methodology

### Motivation: Why the previous `test_end_end` approach was flawed

The previous benchmark used `algorithm_test_end_end`, which toggled the heater ON/OFF every 10ms for 5,000 iterations. This approach had three critical problems:

1. **Toggling too fast for the physical loop to respond.** The simulator runs at 20Hz (50ms tick). A 10ms toggle reverses the heater command before the simulator even processes the previous one. The result: E2E detection measured accumulated thermal drift over many toggle cycles, not single-event signal propagation.
2. **Extremely high variance.** Previous results showed std > mean (CoV > 1.0) — Native: 1,291,578 ± 1,799,666 μs; WAMR: 1,736,285 ± 2,813,567 μs. Values on the order of seconds are clearly dominated by thermal dynamics, not signal latency.
3. **Data race.** `current_temp` was read without `temp_mutex` in `native_set_heater()` / `host_set_heater()` when recording `temp_at_command`, producing potentially torn or stale baseline values.

### Step-response design

The new `algorithm_e2e_step_response` algorithm addresses all three issues:

**Phase 1 — Thermal stabilisation (5s):**
The heater is held OFF for 500 iterations (5s at 10ms period). The simulated temperature settles to ambient (~25°C), establishing a known, stable baseline. Temperature reads and loop metrics are collected throughout this phase.

**Phase 2 — Repeated step responses (6 cycles × 15s each):**
Each cycle consists of:

1. **Heater ON step.** A single call to `set_heater(1.0)` records the current timestamp (`command_send_time_us`) and the current temperature (`temp_at_command`) under proper mutex protection, then sets `waiting_for_rise = true`. This arms the E2E detector in `reader_task`.
2. **Heating hold (3s).** The heater stays ON for 300 iterations. The `reader_task` on Core 1 continuously samples ADC temperature at ~1kHz. When it detects that temperature has risen more than `E2E_THRESHOLD` (0.5°C) above `temp_at_command`, it records `e2e_delay = now - command_send_time_us` and disarms the trigger.
3. **Heater OFF step.** The heater is turned OFF.
4. **Cooldown (12s).** The heater remains OFF for 1200 iterations. The 12s cooldown ensures the temperature returns to ambient before the next cycle, giving each measurement an independent baseline.

### Signal propagation path measured by E2E

The `e2e_time_us` metric captures the full closed-loop round-trip latency:

```
Controller set_heater(1.0)     ─── timestamp captured (command_send_time_us)
  │
  ├─ DAC write: dac_oneshot_output_voltage() on GPIO26
  │
  ├─ Analog signal propagation: Controller DAC → Simulator ADC (GPIO32)
  │
  ├─ Simulator ADC read (next 20Hz tick, 0–50ms wait)
  │
  ├─ Simulator physics computation (temperature update)
  │
  ├─ Simulator DAC write on GPIO25
  │
  ├─ Analog signal propagation: Simulator DAC → Controller ADC (GPIO32)
  │
  ├─ Controller reader_task ADC read (next 1ms poll, 0–1ms wait)
  │
  ├─ Temperature crosses E2E_THRESHOLD (0.5°C above baseline)
  │
  └─ e2e_delay recorded  ─── timestamp captured (end_time)
```

The dominant variable in this path is the simulator's 50ms tick period: depending on when the heater command arrives relative to the simulator's next ADC read, the first physics response can occur after 0–50ms. Subsequent ticks accumulate temperature until the 0.5°C threshold is crossed.

### Fixes applied to E2E instrumentation

1. **Data race eliminated.** `current_temp` is now read under `temp_mutex` before entering the `metrics_mutex` section, stored in a local `safe_temp` variable.
2. **Timestamp moved before DAC write.** `command_send_time_us` is captured before `dac_oneshot_output_voltage()`, so E2E includes the DAC write latency.
3. **Threshold lowered to 0.5°C.** Reduces the number of simulator ticks needed to trigger detection, measuring signal propagation rather than thermal accumulation.

## Metric Definitions

### exec_time_us
Total wall-clock execution time of the entire algorithm, measured once. Captured as `esp_timer_get_time()` delta around the algorithm function call in `run_algorithm_task` (Native) or `wasm_thread_entry` (WAMR). For WAMR, this includes SPIFFS mount, WAMR runtime initialisation, module loading, instantiation, and execution. Expected value: ~95s (5s + 6 × 15s = 95s of `vTaskDelay`-based timing).

### loop_time_us
Time between consecutive `native_delay()` / `host_delay()` calls, measured inside the delay function. Represents one full control loop iteration including temperature read, metric recording, and the preceding delay. Stored in a circular buffer indexed by `loop_iterations % 500`.

### adc_time_us
Time for a single ADC read cycle in `reader_task`: `adc_oneshot_read()` + `adc_cali_raw_to_voltage()` + voltage-to-temperature conversion + `temp_mutex` acquire/release + `metrics_mutex` acquire/release and metrics recording. Measured as the full elapsed time from `reader_task` loop start to `metrics_mutex` release.

### heater_time_us
Time for `native_set_heater()` / `host_set_heater()`: `temp_mutex` acquire for safe temperature read, timestamp capture, value clamping, DAC voltage output via `dac_oneshot_output_voltage()`, and E2E tracking logic under `metrics_mutex`. In WAMR, this additionally includes the WASM-to-native function call boundary crossing (interpreter dispatch, argument marshalling). Only 12 samples are produced (6 ON + 6 OFF calls from the step-response design).

### temp_get_time_us
Time for `native_get_temperature()` / `host_get_temperature()`: acquires `temp_mutex`, reads `current_temp` float, releases mutex. In WAMR, includes interpreter-to-host function call overhead. 500 samples captured (from the circular buffer of the last 500 of ~9,500 total iterations).

### e2e_time_us
End-to-end signal propagation delay from heater command to observable temperature rise. Triggered when `set_heater(>0.5)` is called (records `command_send_time_us` and `temp_at_command` under `temp_mutex`). Completed when `reader_task` observes temperature rise > `E2E_THRESHOLD` (0.5°C) above `temp_at_command`. 6 samples produced, one per step-response cycle.

### free_heap
`esp_get_free_heap_size()` sampled in `reader_task` at each ADC read. Reflects total available heap across all memory regions.

### cpu_core0 / cpu_core1 / cpu_overall
Derived from FreeRTOS runtime stats. Every 10ms, `calculate_cpu_usage` calls `uxTaskGetSystemState()` to get cumulative runtime ticks for all tasks. It extracts the idle task handles for Core 0 and Core 1, computes delta idle ticks since last measurement, and calculates: `core_usage = 100% - (delta_idle / delta_total * 100%)`. Overall is the mean of both cores: `(core0 + core1) / 2`.

### temp_mutex_wait / metrics_mutex_wait
Time in microseconds the `reader_task` (Core 1) spends blocked in `xSemaphoreTake()` waiting to acquire `temp_mutex` and `metrics_mutex` respectively. Measured as `esp_timer_get_time()` delta immediately before and after the semaphore take call. Baseline is 4–8μs depending on timer resolution and function call overhead.

## Results Summary

| Metric | Native | WAMR | WAMR/Native Ratio | Notes |
|--------|--------|------|-------------------|-------|
| **exec_time_us** | 95,004,219 (95.0s) | 114,141,044 (114.1s) | **1.20x** | WAMR 20% slower overall. WAMR includes runtime init + SPIFFS + module load |
| **loop_time_us** (mean) | 10,000 ± 247 | 12,000 ± 0.6 | **1.20x** | Native ~10ms loops; WAMR ~12ms. WAMR has near-zero jitter |
| **adc_time_us** (mean) | 76.74 ± 5.50 | 73.52 ± 6.26 | 0.96x | Identical code on Core 1; within noise — no measurable system-level effect |
| **heater_time_us** (mean) | 20.67 ± 1.56 | 136.50 ± 4.89 | **6.60x** | WASM→native function call boundary overhead (n=12 samples each) |
| **temp_get_time_us** (mean) | 8.03 ± 0.18 | 127.32 ± 0.47 | **15.85x** | Pure mutex read — overhead is entirely WASM→native call dispatch |
| **e2e_time_us** (mean) | 22,350 ± 13,888 | 34,108 ± 19,636 | **1.53x** | Signal propagation latency. 6 samples each. See E2E analysis below |
| **free_heap** (mean) | 253,980 | 85,589 | **0.34x** | WAMR consumes ~168KB for interpreter, module, linear memory |
| **cpu_core0** (mean) | 0.41% | 3.18% | **7.84x** | WAMR interpreter overhead on algorithm core |
| **cpu_core1** (mean) | 17.39% | 25.70% | **1.48x** | Identical tasks; overhead from system-level effects |
| **cpu_overall** (mean) | 8.90% | 14.42% | **1.62x** | ~1.6x overall CPU cost for WAMR |
| **temp_mutex_wait** (mean) | 8.00 ± 0.00 | 4.00 ± 0.04 | 0.50x | No contention in either case; difference is timer resolution |
| **metrics_mutex_wait** (mean) | 8.00 ± 0.00 | 4.59 ± 0.49 | 0.57x | No contention; both at baseline function call cost |

## E2E Results: Detailed Analysis

### Raw E2E Samples

| Step | Native (μs) | WAMR (μs) |
|------|-------------|-----------|
| 1 | 34,845 | 23,914 |
| 2 | 11,127 | 23,941 |
| 3 | 34,853 | 23,940 |
| 4 | 34,853 | 35,940 |
| 5 | 12,709 | 72,973 |
| 6 | 5,714 | 23,942 |
| **Mean** | **22,350** | **34,108** |
| **Std** | **13,888** | **19,636** |
| **CoV** | **0.62** | **0.58** |
| **Min** | **5,714** | **23,914** |
| **Max** | **34,853** | **72,973** |

### Comparison with previous `test_end_end` results

| Aspect | Old (`test_end_end`) | New (`e2e_step_response`) | Improvement |
|--------|---------------------|--------------------------|-------------|
| Native E2E mean | 1,291,578 μs (1.29s) | 22,350 μs (22.4ms) | **58x lower** |
| Native E2E std | 1,799,666 μs | 13,888 μs | **130x lower** |
| Native CoV (std/mean) | 1.39 | 0.62 | **Coefficient of variation halved** |
| WAMR E2E mean | 1,736,285 μs (1.74s) | 34,108 μs (34.1ms) | **51x lower** |
| WAMR E2E std | 2,813,567 μs | 19,636 μs | **143x lower** |
| WAMR CoV (std/mean) | 1.62 | 0.58 | **Coefficient of variation < 1** |
| Sample count | ~22 (noisy, unreliable) | 6 (clean, independent) | Controlled experiment |

The old test measured thermal settling time (seconds) because its 10ms toggle rate was faster than the simulator's 50ms tick — the heater reversed before the simulator could respond. The new test measures actual signal propagation latency (milliseconds) by issuing clean steps from stable baselines with ample time for the system to respond and then cool down.

### Interpreting E2E variance

The remaining variance (CoV ~0.6) is attributable to the simulator's discrete 50ms tick alignment:

- **Best case** (~5.7ms native): Heater command arrives just before a simulator tick. The simulator immediately processes the step, the physics response propagates on the same tick, and the controller's 1kHz ADC reader catches the change within 1–2ms.
- **Worst case** (~35ms native, ~73ms WAMR): Heater command arrives just after a simulator tick, requiring up to 50ms before the simulator reads the new heater state. If the first physics tick produces < 0.5°C rise, an additional 50ms tick is needed to cross the threshold.

Native values cluster bimodally around ~5–13ms (fortunate tick alignment) and ~35ms (one full tick wait). WAMR values cluster around ~24ms (consistent half-tick latency) with one outlier at ~73ms (requiring two simulator ticks to cross threshold, likely due to interpreter-induced timing shift in the DAC write).

### WAMR E2E overhead

The WAMR E2E mean is 1.53x the native mean (34.1ms vs 22.4ms). This 11.8ms additional latency consists of:

1. **Host function call overhead in `set_heater`.** The WASM→native boundary adds ~116μs per heater call (136μs WAMR − 21μs native). This shifts the DAC write ~116μs later relative to the simulator tick, making unfavourable tick alignment slightly more likely.
2. **Interpreter latency in loop execution.** Each WAMR loop iteration takes 2ms longer (12ms vs 10ms). While the E2E timer is not directly tied to the loop, the additional latency in the iteration where `set_heater(1.0)` is called delays when the command reaches the DAC.
3. **Tick alignment sensitivity.** The 50ms simulator tick granularity amplifies small timing differences. An additional 2ms delay in issuing the heater command can shift the measurement from "catches the current tick" to "waits for the next tick", adding a full 50ms.

## Analysis

### Host Function Call Overhead

The most significant per-call overhead is in WASM-to-native function calls. `temp_get_time_us` shows a 15.85x ratio (8μs → 127μs), and `heater_time_us` shows 6.60x (21μs → 137μs). These functions perform identical hardware operations. The overhead comprises:

1. **WAMR interpreter dispatch**: The interpreter must decode the WASM `call` instruction, look up the registered native symbol, validate the function signature, and marshal arguments from the WASM operand stack to native C calling convention.
2. **Execution environment context**: Each host function receives a `wasm_exec_env_t` parameter, requiring the interpreter to prepare and pass the execution context.
3. **Return value marshalling**: Results must be converted back from native types to WASM operand stack values.

The `temp_get_time_us` measurement is the cleanest overhead indicator: the underlying operation (mutex take, float read, mutex give) takes a constant 8μs natively, making the additional ~119μs purely attributable to the WASM call boundary.

Note: `heater_time_us` now includes an additional `temp_mutex` acquire/release for the data-race fix (safe `temp_at_command` read). This adds ~4–8μs to native (14→21μs vs the previous benchmark), but has negligible proportional impact on WAMR.

### Loop Time Overhead

Native achieves 10,000μs mean loop time (matching the configured 10ms `PERIOD_MS`). WAMR measures 12,000μs — 2ms additional per iteration. Each step-response iteration performs 1x `host_get_temperature` + 1x `host_record_temp_get_time` + 1x `host_delay` call. The accumulated per-call overhead (~119μs + ~119μs + ~119μs ≈ 357μs) accounts for a portion; the remainder is attributable to WASM bytecode interpretation between host calls (loop counter management, conditional branching, stack operations) and `vTaskDelay` tick rounding at different base times.

Notable: WAMR loop time has near-zero jitter (std = 0.60μs) compared to native (std = 247μs). This is likely because the interpreter's consistent overhead creates a more deterministic execution time per iteration, and the 12ms total duration lands cleanly on a FreeRTOS tick boundary.

### CPU Usage

**Core 0** (algorithm core): Native 0.41% vs WAMR 3.18% (7.84x). This directly reflects the interpreter's computational overhead — WAMR performs significantly more instructions per control loop iteration (bytecode fetch-decode-execute cycle for each WASM opcode, plus host call marshalling).

**Core 1** (monitoring core): Native 17.39% vs WAMR 25.70% (1.48x). This core runs identical code in both configurations. The overhead is attributed to `uxTaskGetSystemState()` taking longer per call in the WAMR environment due to increased system complexity (additional pthread management, WAMR internal state) affecting FreeRTOS kernel data structure traversal time. This is confirmed by the CPU overhead investigation experiment (see `experiments/cpu_overhead_investigation.md`).

### ADC Read Time

The ADC reader task runs identical code on Core 1 in both controllers and shows effectively identical performance: 76.74μs (Native) vs 73.52μs (WAMR), a 0.96x ratio within measurement noise. This is an improvement over the previous benchmark (which showed a 1.25x ratio) — the step-response algorithm's lower call frequency into `set_heater` reduces any transient contention on shared resources.

### Memory Footprint

WAMR consumes ~168KB of heap (254KB → 86KB free). This includes:
- WAMR runtime engine structures
- WASM module instance (operand stack: 16KB, app heap: 16KB)
- WASM linear memory (64KB as defined in compilation flags `--initial-memory=65536`)
- Execution environment (8KB)
- SPIFFS filesystem buffers

Native free heap is constant at 253,980 bytes (zero variance). WAMR shows minor fluctuation (std = 12.7 bytes) from allocator metadata bookkeeping.

### Mutex Contention

Both `temp_mutex_wait` and `metrics_mutex_wait` are at the baseline `xSemaphoreTake()` function call cost (4–8μs depending on timer resolution) in both configurations. There is zero contention. The difference between 8μs (native) and 4μs (WAMR) is an `esp_timer_get_time()` resolution artifact — both represent a single uncontested `xSemaphoreTake` call with no blocking.

## Conclusions

1. **WAMR adds 20% total execution overhead** for this workload (95s → 114s), consistent across both the old and new test algorithms, confirming this is a fundamental characteristic of the interpreter-based runtime rather than an artifact of the test design.

2. **Per-call host function overhead is 119–136μs** (15.85x for temperature reads, 6.60x for heater writes). This is the dominant cost of the WASM sandbox boundary and is inherent to the WAMR interpreter mode. For control applications, this bounds the achievable loop rate: at ~360μs overhead per iteration (3 host calls), the maximum practical control frequency is ~2.8kHz before overhead alone consumes one tick.

3. **E2E signal propagation latency is 22–34ms**, not 1.3–1.7s as previously measured. The step-response methodology produces reliable, low-variance measurements (CoV < 0.65) with clear physical interpretation. WAMR adds 53% E2E overhead (34ms vs 22ms), primarily from interpreter-induced timing shifts interacting with the simulator's 50ms tick granularity.

4. **Memory cost is fixed at ~168KB** regardless of algorithm complexity, dominated by the WAMR runtime and the 64KB WASM linear memory allocation.

5. **No mutex contention** exists between the algorithm core (Core 0) and the monitoring core (Core 1). Performance differences are entirely attributable to WAMR interpreter overhead and its system-level effects on FreeRTOS.

## Raw Data Reference

- Native CSV: `Native_controller/results.csv` (500 rows, 13 columns)
- WAMR CSV: `Wasm_controller/results.csv` (500 rows, 13 columns)
- Both files captured via `exec_esp32.py` which extracts data between `<<<CSV_START>>>` and `<<<CSV_END>>>` serial markers
- Analysis scripts: `Native_controller/analyse.py`, `Wasm_controller/analyse.py`, `compare_stats.py`
- Previous benchmark (old methodology): results retained in git history for reference
