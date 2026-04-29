# Project Overview for Writing

This file is a handoff brief for any agent helping write or edit the report in this repository.

## One-paragraph summary

This project evaluates whether WebAssembly can be used as a practical modular execution layer for embedded control workloads on an ESP32. The runtime used is WAMR (WebAssembly Micro Runtime), tested in both interpreter and Ahead-of-Time (AOT) modes. The evaluation is not limited to micro-benchmarks: it uses a two-board hardware-in-the-loop thermal-control setup, compares native C against WAMR interpreter and WAMR AOT, and then extends the analysis to fault containment, update strategy, inter-container communication, and multi-container scaling.

## Core research question

The main question is whether Wasm on an ESP32 is light enough to be useful for real embedded workloads, while still providing something valuable in return.

That breaks into two practical sub-questions:

1. What does WAMR cost in timing, CPU, and memory?
2. What system-level benefits does it provide in modularity, isolation, and update flexibility?

## Main contribution of the project

The report argues that Wasm is viable on this class of device, but the answer depends strongly on execution mode.

- WAMR interpreter is workable, but CPU overhead is much higher.
- WAMR AOT is the practical deployment mode.
- The main runtime cost is SRAM and host/runtime overhead, not gross control-loop failure.
- In return, Wasm gives meaningful isolation, lighter logic-only updates, and more flexible partitioning of application logic into containers.

## High-level report structure

The report is in `report/report.tex`.

Current chapter flow:

1. Introduction
2. Background and Context
3. Design and Implementation
4. Performance profiling
5. Fault tolerance
6. Containerization strategies
7. Conclusions

Chapter 6 now contains:

- `6.1` Over-the-air updates
- `6.2` Inter-container communication
- `6.3` Container-count scaling
- `6.4` Discussion

## Writing style to preserve

The report already has a clear style. Follow it closely.

- Write in formal but direct technical prose.
- Prefer short, concrete claims over broad framing.
- Avoid exaggerated language.
- Avoid obvious AI-style padding, especially words like "showcases", "underscores", "highlights", "vibrant", "crucial" unless genuinely needed.
- Use British spelling where the report already does: `behaviour`, `organised`, etc.
- Base conclusions on measured data, not impressions.
- When discussing trade-offs, say exactly what the limiting factor is.
- If a result is shaped by instrumentation or benchmark structure, say so plainly.

## Tone and argument pattern used in the report

Most result sections follow this pattern:

1. Explain what was measured and why.
2. Present the main numbers.
3. Interpret what those numbers mean operationally.
4. Tie the interpretation back to embedded constraints: timing budget, SRAM budget, scheduler behaviour, recovery granularity, update payload, and so on.

This report does not treat Wasm as an abstract software-engineering idea. It treats it as an engineering trade-off on a constrained device.

## System under test

The target platform is the ESP32 running ESP-IDF and FreeRTOS.

The project uses a two-board hardware-in-the-loop thermal testbed:

- one board acts as the controller
- one board acts as the plant/simulator
- the setup allows repeatable thermal-control experiments with ADC/DAC paths and real RTOS scheduling overhead

The main execution variants are:

- native C
- WAMR interpreter
- WAMR AOT

## Main codebase layout

Relevant top-level directories:

- `Native_controller/`
- `Wasm_controller/`
- `ipc_benchmark/`
- `report/`

### `Native_controller/`

Native baseline firmware for the controller workload.

### `Wasm_controller/`

WAMR-based controller firmware for the main control experiments. This contains the controller workloads compiled as Wasm containers and executed through WAMR.

### `ipc_benchmark/`

Separate benchmark project for multi-container work:

- IPC method benchmarks
- low-memory scaling benchmark
- SPIFFS-hosted Wasm/AOT worker modules

This is where the multi-container scaling benchmark was implemented.

### `report/`

Main LaTeX report and images.

## Key experiments and what they show

### 1. Control-performance comparison

This part asks whether the controller still behaves acceptably under WAMR.

Main conclusion:

- At the tested 100 ms control period, both interpreter and AOT preserve control behaviour for the thermal workload.
- The cost is visible in fine-grained overhead and memory, not in a catastrophic failure of the control task.

Key abstract-level figures already used in the report:

- interpreter added about `59 us` to a temperature read and `113 us` to a heater write over the native baseline
- AOT reduced those penalties to about `23-25 us`
- total wall-clock overhead stayed below `0.2%` because the workloads were dominated by deliberate delays
- WAMR consumed about `76-86 KB` of additional SRAM compared with native

### 2. Fault injection and recovery

This part asks whether the runtime changes failure behaviour in a meaningful way.

Fault classes tested:

- null pointer
- stack overflow
- infinite loop

Main conclusion:

- Native faults reset the chip.
- WAMR faults are contained to the instance and the host survives.
- Every WAMR fault class tested was recovered without rebooting the device.

Important conceptual distinction:

- Native recovery is system-level reboot and restart.
- WAMR recovery is container-level re-instantiation.

That difference in recovery granularity is one of the strongest system-level arguments in the project.

### 3. Over-the-air update strategy

This part compares full firmware flashing against SPIFFS-only container updates.

Main result:

- SPIFFS-only update: about `7.60 s`
- full flash: about `20.15 s`
- build + flash: about `59.04 s`

Interpretation:

- If only the control logic changes, a Wasm container update is substantially lighter than reflashing the full image.
- The payload is also smaller, which matters for fleet rollout cost.

### 4. Inter-container communication

This benchmark isolates the cost of different IPC architectures.

Architectures studied:

- shared memory
- queue
- stream buffer
- host-mediated read
- host-driven invocation

Main conclusions:

- architecture choice dominates payload size
- host-mediated read is the cheapest useful isolated design
- queue and stream buffer pay heavily in tail latency because the scheduler is involved
- host-driven invocation is the most expensive but also very consistent

The report uses the phrase "tail cost" intentionally. For control loops, worst-case behaviour matters more than average cost.

### 5. Container-count scaling

This is the new Chapter `6.3` section added later in the project.

It measures how the system behaves as the number of concurrently active Wasm containers increases.

Main benchmark design:

- one simple worker module
- same worker loaded `N` times
- one module instance, one exec env, and one host pthread per worker
- workers released together
- benchmark run in interpreter and AOT mode
- sweep stops at first failed or unstable `N`

Important methodological point:

This benchmark measures stable concurrent execution, not just "how many instances can be allocated".

That distinction matters because interpreter mode hits a scheduler/CPU limit before it hits a memory limit.

#### Final scaling results used in the report

Interpreter:

- highest stable `N = 6`
- first failing `N = 7`
- failure mode: timeout / task-watchdog-triggered instability

AOT:

- highest stable `N = 11`
- first failing `N = 12`
- failure mode: `pthread_failed`

#### Why interpreter fails earlier

Not because heap is exhausted.

At interpreter `N = 7`, the benchmark still had substantial free heap during steady state, but the workers were spending too much time inside WAMR's bytecode interpreter and starving the idle task. So the limiting factor is scheduler starvation and CPU saturation, not raw allocation failure.

This is one of the most important distinctions in the whole report:

- "How many containers fit in memory?" is not the same as
- "How many containers run stably under this workload?"

#### Latest scaling numbers used in the report

From `ipc_benchmark/results/scaling/results.csv`:

Interpreter successful rows:

- `N=1`: heap after instantiation `250476 B`, steady heap `239388 B`, CPU overall `34.97%`
- `N=2`: `243508 B`, `221468 B`, `63.94%`
- `N=3`: `236600 B`, `203544 B`, `68.15%`
- `N=4`: `229712 B`, `185640 B`, `92.95%`
- `N=5`: `222808 B`, `167720 B`, `82.63%`
- `N=6`: `215904 B`, `149800 B`, `99.13%`
- `N=7`: timeout, steady heap still `131880 B`

AOT successful rows:

- stable through `N=11`
- `N=11`: heap after instantiation `125372 B`, steady heap `4012 B`, CPU overall `18.11%`
- `N=12`: `pthread_failed`

#### Slopes already used in the report

The section uses slope-based analysis rather than just visual impressions.

Approximate fitted slopes over successful runs:

Interpreter:

- heap after instantiation: about `6.9 kB` lost per added container
- steady-state heap: about `17.9 kB` lost per added active container
- startup time: about `3.3 ms` added per container
- CPU overall: about `11.5 percentage points` added per container

AOT:

- heap after instantiation: about `12.0 kB` lost per added container
- steady-state heap: about `23.0 kB` lost per added active container
- startup time: about `5.2 ms` added per container
- CPU overall: about `1.57 percentage points` added per container

Interpretation:

- AOT is much better on CPU.
- AOT is not cheaper in RAM for this benchmark. It is more expensive per active container in memory, but it executes efficiently enough that memory becomes the first limit instead of CPU.

## Important conceptual distinctions that should stay consistent

When writing, keep these distinctions clear.

### "Native" vs "WAMR"

- Native means normal C code compiled into the firmware image.
- WAMR means code running as a Wasm module under the runtime.

### Interpreter vs AOT

- Interpreter executes Wasm bytecode through WAMR's interpreter loop.
- AOT executes precompiled native code generated from the Wasm module.

Do not blur those together. Many of the report's conclusions depend on the difference.

### "Single-module cost" vs "multi-container system behaviour"

The earlier profiling chapters measure a single module.
The later containerisation chapter measures how a modular architecture behaves at system level.

### "Fits in memory" vs "runs stably"

Especially in the scaling section, do not imply that spare heap automatically means more containers are safe.

Interpreter mode disproves that.

### "Fault detected" vs "fault recovered"

The report often separates detection latency from recovery latency. Keep that distinction.

## Files most useful for writing

### Main report

- `report/report.tex`

### Scaling benchmark data

- `ipc_benchmark/results/scaling/results.csv`
- `ipc_benchmark/results/scaling/results_summary.csv`
- `ipc_benchmark/results/scaling/results_wdt.log`
- `ipc_benchmark/results/scaling/results_runtime_stats.csv`

### IPC benchmark code

- `ipc_benchmark/main/ipc_benchmark.c`

### Scaling benchmark code

- `ipc_benchmark/main/wamr_scaling_benchmark.c`
- `ipc_benchmark/containers/scaling_worker.c`
- `ipc_benchmark/main/scaling_worker_config.h`
- `ipc_benchmark/run_scaling_benchmark.bash`

### Main controller projects

- `Native_controller/`
- `Wasm_controller/`

## If you are asked to write a new subsection

Use this workflow:

1. Read the surrounding subsection in `report/report.tex`.
2. Match the paragraph length and tone already there.
3. Pull exact numbers from the relevant CSV or table source.
4. State the mechanism behind the result, not just the number.
5. Be careful about whether the limit is timing, CPU, scheduler, heap, or update payload.
6. Avoid generic "broader significance" sentences unless the chapter already builds to one.

## If you are asked to revise claims

Prefer these kinds of formulations:

- "The limiting factor was scheduler starvation rather than heap exhaustion."
- "AOT reduced runtime overhead enough that memory, not CPU, became the first hard limit."
- "The result is meaningful because the host and long-lived tasks remain alive while only the container is replaced."

Avoid vague formulations like:

- "This highlights the promise of WebAssembly"
- "The findings underscore a transformative shift"
- "The results reflect a broader trend"

## Best short summary to keep in mind

The project does not claim that Wasm is free on microcontrollers.

It shows something narrower and more useful:

- Wasm is viable for this ESP32 thermal-control workload.
- AOT is the mode that makes it practical.
- The main cost is SRAM and runtime overhead.
- The main benefits are fault containment, cheaper logic-only updates, and more modular firmware structure.
