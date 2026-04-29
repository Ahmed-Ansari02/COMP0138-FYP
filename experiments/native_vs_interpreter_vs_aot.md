# Native vs WAMR Interpreter vs WAMR AOT Comparison

## Data Sources
- **Native**: `Native_controller/results.csv`
- **WAMR Interpreter**: `Wasm_controller/results.csv`
- **WAMR AOT**: `Wasm_controller/results_aot.csv`
- 500 samples each, step response workload (`e2e_step_response`)

---

## Total Execution Time

| | Native | WAMR AOT | WAMR Interpreter |
|--|--------|----------|-----------------|
| `exec_time_us` | **95.0s** | **95.1s** | **114.1s** |
| vs Native | baseline | +0.1% | +20.1% |

AOT is essentially identical to native total execution time. Interpreter is 20% slower.

---

## Loop Time (per iteration)

| | Native | WAMR AOT | WAMR Interpreter |
|--|--------|----------|-----------------|
| `loop_time_us` | **10,000 us** | **~10,000 us** | **12,000 us** |

Both native and AOT hit the 10ms target. Interpreter adds ~2ms overhead per loop from bytecode dispatch.

---

## Host Function / Call Overhead

| Function | Native | WAMR AOT | WAMR Interpreter |
|----------|--------|----------|-----------------|
| `heater_time_us` | **20 us** | **44 us** (2.2x) | **138 us** (6.9x) |
| `temp_get_time_us` | **8 us** | **32 us** (4x) | **130 us** (16.3x) |

The WASM boundary crossing cost:
- AOT adds ~24us per host call for heater, ~24us for temp get
- Interpreter adds ~118us and ~122us respectively -- bytecode dispatch overhead dominates
- This is the most significant per-call overhead and the main differentiator between execution modes

---

## Memory (Free Heap)

| | Native | WAMR AOT | WAMR Interpreter |
|--|--------|----------|-----------------|
| `free_heap` | **253,980 bytes** | **100,592 bytes** | **85,588 bytes** |
| WAMR overhead | -- | **-153 KB** | **-168 KB** |

- The WAMR runtime itself costs ~153KB (AOT) or ~168KB (interpreter) of heap
- That is ~60-65% of the ESP32's available RAM consumed by the runtime
- The extra 15KB in interpreter mode is for the bytecode decoder and operand stack
- This is the primary practical limitation of running WASM on ESP32

---

## CPU Usage

| Core | Native | WAMR AOT | WAMR Interpreter |
|------|--------|----------|-----------------|
| Core 0 (algo) | **0.40%** | **0.83%** | **3.08%** |
| Core 1 (ADC/tasks) | **17.39%** | **25.71%** | **25.78%** |
| Overall | **8.89%** | **13.28%** | **14.43%** |

- Core 0: AOT is 2x native, interpreter is 7.7x native -- but all very low since `host_delay` dominates
- Core 1 is ~8% higher with WAMR vs native across both modes, despite only running ADC/metrics tasks
- The Core 1 increase is likely caused by higher mutex contention -- WAMR host calls change the timing relationship with the ADC reader task

---

## Mutex Wait Times

| | Native | WAMR AOT | WAMR Interpreter |
|--|--------|----------|-----------------|
| `temp_mutex_wait` | **8 us** | **4-5 us** | **4-5 us** |
| `metrics_mutex_wait` | **8 us** | **4-5 us** | **4-5 us** |

WAMR mutex waits are lower than native. The WAMR host calls are slower, so the reader task encounters less contention (the WASM thread holds mutexes less frequently).

---

## Key Takeaways

1. **AOT nearly eliminates the execution time gap** -- only 0.1% slower than native overall
2. **The real cost of WASM is memory** -- 153-168KB runtime overhead on a 520KB device is the main practical limitation
3. **Per-call overhead is the differentiator** -- 24us (AOT) vs 0us (native) per host function call. For high-frequency control loops this matters
4. **Core 1 CPU increase with WAMR is unexpected** and worth investigating -- suggests the WAMR runtime affects system-wide scheduling even for tasks on other cores
5. **Interpreter adds 2ms per loop iteration** -- for a 10ms control loop this is a 20% timing error, which could impact control quality
6. **AOT achieves near-native loop timing precision** -- important for deterministic control systems
