# CPU Overhead Investigation: WAMR vs Native on ESP32

## Observation

When benchmarking the same `test_end_end` control algorithm running as WAMR-interpreted WebAssembly vs native C on ESP32, Core 1 CPU usage was significantly higher in the WAMR configuration despite running identical tasks on that core.

| Metric | Native | WAMR |
|--------|--------|------|
| Core 0 (algorithm) | ~0.5–1.0% | ~3.3–5.4% |
| Core 1 (ADC reader + monitoring) | ~15.1% | ~24.2% |
| Overall | ~7.8% | ~14.1% |

The Core 0 difference is expected — WAMR interprets bytecode rather than executing native instructions. The Core 1 difference is unexpected since both controllers pin the same tasks to Core 1.

### Task Pinning (identical in both controllers)

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| Control algorithm / WASM Runtime | Core 0 | 5 | Runs control loop |
| ADC Reader | Core 1 | 5 | Reads temperature via ADC at ~1kHz |
| CPU Usage | Core 1 | 3 | Calls `uxTaskGetSystemState()` every 10ms |
| Metrics (disabled) | Core 1 | 2 | Periodic logging |

## FreeRTOS Runtime Stats

Per-task runtime breakdown from `vTaskGetRunTimeStats()`:

### Native
| Task | Abs Ticks | % Time |
|------|-----------|--------|
| ADC Reader | 8,537,659 | 8% |
| control algorithm | 2,637,738 | 2% |
| IDLE1 | 86,996,135 | 85% |
| IDLE0 | 99,546,088 | 97% |
| CPU Usage | 6,674,025 | 6% |

### WAMR
| Task | Abs Ticks | % Time |
|------|-----------|--------|
| ADC Reader | 12,074,243 | 9% |
| WASM Runtime | 7,429,176 | 6% |
| IDLE1 | 90,801,163 | 74% |
| IDLE0 | 115,013,110 | 93% |
| CPU Usage | 19,571,870 | 15% |

**Key finding**: The `CPU Usage` task on Core 1 consumes 3x more runtime under WAMR (6% → 15%). The ADC Reader also increases by ~41%. Both tasks are identical in code — the overhead comes from the system environment, not the tasks themselves.

## Hypothesis 1: Mutex Contention

**Theory**: WAMR's slower interpreted execution holds shared mutexes (`temp_mutex`, `metrics_mutex`) longer on Core 0, causing the reader task on Core 1 to block waiting for locks.

### Experiment

Added `esp_timer_get_time()` instrumentation around every `xSemaphoreTake()` call in `reader_task()` to measure how long Core 1 blocks waiting for each mutex. Results output as `temp_mutex_wait` and `metrics_mutex_wait` CSV columns.

### Results

| Metric | Native | WAMR |
|--------|--------|------|
| `temp_mutex_wait` | 4μs (constant) | 4–5μs |
| `metrics_mutex_wait` | 4μs (constant) | 4μs (constant) |

Both values represent the baseline cost of the `xSemaphoreTake()` call itself — zero actual contention.

### Conclusion

**Ruled out.** Mutex wait times are identical and near-zero in both configurations. The reader task never actually blocks on either mutex.

## Hypothesis 2: Static Memory Pressure

**Theory**: WAMR allocates ~167KB for its runtime (interpreter state, operand stack, linear memory), reducing free heap from 254KB to 87KB. This reduced heap may cause slower memory allocator behaviour or fragmentation effects that impact Core 1 tasks.

### Experiment

Added `malloc(167 * 1024)` at the start of Native's `app_main()` to consume the same amount of heap without actively using it. This isolates the effect of reduced available memory from active access patterns.

### Results

| Metric | Native (baseline) | Native + 167KB malloc | WAMR |
|--------|-------------------|----------------------|------|
| Core 1 | 15.1% | 16.25% | 24.2% |
| Free heap | 253,980 | ~87,000 | 86,744 |

### Conclusion

**Minor factor (~1%).** Static memory pressure accounts for roughly 1 percentage point of the ~9% Core 1 gap. The reduced heap alone does not replicate WAMR's overhead.

## Hypothesis 3: Shared Memory Bus Contention

**Theory**: WAMR's interpreter continuously reads/writes its ~167KB working set on Core 0. On ESP32's shared memory bus, this active traffic competes with Core 1's memory accesses, slowing down `uxTaskGetSystemState()` and ADC reads.

### Experiment

Created a `mem_thrash_task` pinned to Core 0 that allocates 64KB (largest safe contiguous block) and continuously performs pseudo-random read/write operations across the buffer, simulating interpreter-like memory access patterns.

### Results

| Metric | Native (baseline) | Native + mem_thrash | WAMR |
|--------|-------------------|---------------------|------|
| Core 0 | ~0.55% | 18.51% | ~4.4% |
| Core 1 | 16.25% | 17.10% | 24.2% |

### Conclusion

**Minor factor (~1%).** Random data memory access on Core 0 increases Core 1 usage by less than 1 percentage point, even with aggressive thrashing. Simple memory bus contention from data access does not explain the overhead.

**Note**: The thrash task drove Core 0 to 18.5% (higher than WAMR's ~4.4%), yet Core 1 barely moved. This demonstrates that raw memory bandwidth consumption on Core 0 has minimal cross-core impact on ESP32.

## Hypothesis 4: Core Pinning the Workload

**Theory**: If the Core 1 overhead is caused by the WAMR interpreter competing for shared resources on Core 0, moving the workload to Core 1 should shift the overhead and reveal whether it follows the interpreter or is tied to the core.

### Experiment

Ran both Native and WAMR with the control algorithm pinned to Core 0 (default) and then to Core 1, collecting 500 samples in each configuration.

### Results

| Metric | Native (Core 0 pin) | Native (Core 1 pin) | WAMR (Core 0 pin) | WAMR (Core 1 pin) |
|--------|---------------------|---------------------|--------------------|--------------------|
| Core 0 | 0.41% | 0.41% | 3.04% | 1.22% |
| Core 1 | 17.4% | 17.4% | 27.80% | 28.77% |
| ADC time | 77 us | 77 us | 85 us | 91 us |
| Metrics mutex wait | 8 us | 8 us | 20 us | 16 us |
| Loop time | 10,000 us | 10,000 us | 10,000 us | 10,336 us |

### Analysis

1. **Core 1 overhead persists regardless of workload pinning.** Moving the WAMR algorithm from Core 0 to Core 1 only increased Core 1 from 27.8% to 28.8% (+1%). The ~10% gap vs Native (17.4%) exists in both configurations. This confirms the overhead is not caused by the control algorithm competing for Core 1 time.

2. **The overhead is systemic.** The reader task and CPU measurement task on Core 1 are identical C code in both Native and WAMR, yet they run measurably slower under WAMR — ADC reads take 85–91 us vs 77 us in Native, and metrics mutex waits are 16–20 us vs 8 us.

3. **Shared memory bus / cache pressure is the mechanism.** The WAMR runtime consumes ~127 KB more RAM, and the interpreter performs heavy, irregular memory access patterns (bytecode fetch, operand stack manipulation, indirect dispatch tables). On ESP32's shared bus architecture, this increases memory access latency for Core 1's tasks even though they run on a separate core. The ~10% slower ADC reads are consistent with peripheral register accesses going through the same bus.

4. **Co-locating workload with reader degrades loop time.** When the WAMR interpreter moves to Core 1 alongside the reader task, loop time increases from 10,000 to 10,336 us — the interpreter is CPU-heavy enough to cause scheduling contention when sharing a core.

### Conclusion

**Confirms systemic shared-resource overhead.** The Core 1 CPU increase in WAMR is not caused by direct competition for Core 1 time, but by the interpreter's memory-intensive execution polluting shared hardware resources (memory bus, cache) system-wide. This overhead would likely shrink with AOT compilation, since AOT code has native-like memory access patterns.

## Remaining Overhead (~7%)

After eliminating mutex contention, static memory pressure, and data memory bus contention, approximately 7 percentage points of Core 1 overhead remain unexplained by isolated experiments. The likely causes are characteristics unique to WAMR's interpreter execution that cannot be replicated by simple memory access patterns:

1. **`uxTaskGetSystemState()` scaling**: This FreeRTOS function walks internal task control block (TCB) data structures. Under WAMR, the system has additional thread management overhead (pthread wrapper, WAMR internal state) that may increase the per-call cost of this function non-linearly, even with only one additional task.

2. **Instruction cache/branch predictor pollution**: WAMR's interpreter loop on Core 0 executes a large `switch`-based dispatch over WASM opcodes. This creates highly unpredictable branch patterns and fills the instruction cache with interpreter code. On ESP32, while each core has its own instruction cache, the shared instruction bus may experience contention when both cores fetch instructions simultaneously.

3. **FreeRTOS scheduler interactions**: The WASM pthread and its associated WAMR runtime state add complexity to the scheduler's bookkeeping. Context switches involving the WASM thread may be more expensive due to larger saved state (24KB stack vs 4KB for native tasks).

4. **System allocator fragmentation**: Unlike a single large `malloc`, WAMR performs many small allocations during runtime initialisation and module instantiation. This fragmented allocation pattern may cause the memory allocator's internal data structures to be larger and more scattered, increasing traversal time for any operation that touches heap metadata.

## Summary

| Hypothesis | Experiment | Core 1 Impact | Status |
|---|---|---|---|
| Mutex contention | Wait-time instrumentation | 0% | Ruled out |
| Static memory pressure | 167KB malloc | ~1% | Minor factor |
| Data memory bus contention | 64KB random R/W thrash | ~1% | Minor factor |
| Core pinning | Workload on Core 0 vs Core 1 | +1% when co-located | Confirms systemic overhead |
| WAMR interpreter system effects | — | ~7% | Most likely cause |

The Core 1 CPU overhead in WAMR is an emergent property of the interpreter's execution characteristics interacting with ESP32's shared hardware resources and FreeRTOS internals, rather than any single isolatable bottleneck.
