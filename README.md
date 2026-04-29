# COMP0138 FYP: WebAssembly Containers on ESP32

This repository contains the code used for a final-year project evaluating whether WebAssembly can be used as a practical modular execution layer for ESP32-class IoT devices. The project compares native ESP-IDF firmware against WebAssembly Micro Runtime (WAMR) interpreter and ahead-of-time (AOT) execution using a two-board Hardware-in-the-Loop thermal-control testbed.

The repository includes:

- Native ESP32 controller firmware.
- WAMR-based ESP32 controller firmware.
- ESP32 thermal simulator firmware.
- Wasm container source code and compilation scripts.
- Fault tolerance, hot-swap, update, IPC, CPU, and scaling benchmark scripts.
- Python plotting and analysis scripts.
- The final report and experiment outputs.

## Hardware Setup

The main control experiments use two ESP32 boards:

- Controller board: flashed either with `Native_controller` or `Wasm_controller`.
- Simulator board: flashed with `Simulator` and left running as the thermal plant.

The boards communicate through analogue GPIO wiring:

- Controller DAC output sends the heater command to the simulator ADC input.
- Simulator DAC output sends simulated temperature to the controller ADC input.
- Grounds are connected.
- A reset line is also used in the testbed for controlled reset behaviour.

In the report, this setup is described as a simulated closed-loop thermal-control system. It is not a real heater and sensor plant, but it uses real ESP32 ADC/DAC peripherals, FreeRTOS scheduling, serial logging, and firmware flashing.

## Software Requirements

Install or make available:

- ESP-IDF with `idf.py` available in the shell.
- An ESP32 target configured for ESP-IDF builds.
- WASI SDK installed at `/opt/wasi-sdk`.
- `wamrc` available at `Wasm_controller/compilers/wamrc` for AOT generation.
- Python 3.
- Python packages used by scripts, mainly `pandas`, `matplotlib`, `numpy`, and `pyserial`.

Before running firmware scripts, load the ESP-IDF environment in the shell. For example:

```bash
source /path/to/esp-idf/export.sh
```

If `IDF_PATH` is already set, this can also be written as:

```bash
source "$IDF_PATH/export.sh"
```

Some local setups define a convenience alias or shell function such as `get_idf`, but that is not provided by this repository and is not available unless configured separately.

If multiple ESP32 boards are connected, pass the serial port explicitly to the benchmark scripts. Most scripts also respect `ESPPORT`.

```bash
export ESPPORT=/dev/cu.usbserial-XXXX
```

## Repository Layout

```text
.
├── Native_controller/          Native ESP-IDF controller firmware
├── Wasm_controller/            WAMR ESP-IDF controller firmware and Wasm containers
├── Simulator/                  ESP32 thermal simulator firmware
├── ipc_benchmark/              IPC and multi-container scaling benchmark firmware
├── Benchmarking_scripts/       Automated experiment runners for controller benchmarks
├── Analysis_and_plot_scripts/  Python analysis and plotting scripts
├── Ota_benchmarks/             OTA-like update benchmark script
├── experiments/                Captured experiment data, plots, and write-ups
├── report/                     LaTeX report, figures, bibliography, and generated PDF
└── exec_esp32.py               Serial capture wrapper used by benchmark scripts
```

Generated folders such as `build/`, `build_scaling/`, `managed_components/`, and LaTeX auxiliary files are not source files. They are produced by ESP-IDF, WAMR component fetching, or LaTeX builds.

## Quick Start

1. Flash the simulator board:

```bash
cd Simulator
idf.py build flash monitor
```

2. Compile the Wasm containers:

```bash
cd Wasm_controller/containers
bash create_container.bash
```

3. Flash and monitor the native controller:

```bash
cd Native_controller
idf.py build flash monitor
```

4. Flash and monitor the WAMR controller:

```bash
cd Wasm_controller
idf.py build flash monitor
```

5. Run the automated control-algorithm benchmark from the repository root:

```bash
bash Benchmarking_scripts/run_control_algorithms.bash all both
```

Results are written under:

```text
experiments/control_algorithms/<timestamp>/
```

## Running the Main Experiments

### Control Algorithm Benchmarks

Runs bang-bang, PID, and step-response workloads for native and WAMR variants.

```bash
bash Benchmarking_scripts/run_control_algorithms.bash
```

Useful variants:

```bash
bash Benchmarking_scripts/run_control_algorithms.bash native
bash Benchmarking_scripts/run_control_algorithms.bash wamr wasm
bash Benchmarking_scripts/run_control_algorithms.bash wamr aot
bash Benchmarking_scripts/run_control_algorithms.bash all both /dev/cu.usbserial-XXXX
```

Output:

```text
experiments/control_algorithms/<timestamp>/
```

### Controller Configuration Sweep

Runs the original controller benchmark matrix across end-to-end, hot-swap, and fault modes.

```bash
bash Benchmarking_scripts/run_all_controller_configs.bash
```

Useful variants:

```bash
bash Benchmarking_scripts/run_all_controller_configs.bash native
bash Benchmarking_scripts/run_all_controller_configs.bash wamr
bash Benchmarking_scripts/run_all_controller_configs.bash all /dev/cu.usbserial-XXXX
```

Output:

```text
experiments/controller_config_sweep/<timestamp>/
```

### Full Benchmark Suite

Runs the larger matrix across test modes, controller core placement, and WAMR execution mode. This performs many flashes and takes a long time.

```bash
bash Benchmarking_scripts/run_full_benchmark_suite.bash
```

Useful variants:

```bash
bash Benchmarking_scripts/run_full_benchmark_suite.bash native
bash Benchmarking_scripts/run_full_benchmark_suite.bash wamr
bash Benchmarking_scripts/run_full_benchmark_suite.bash all /dev/cu.usbserial-XXXX
```

Output:

```text
experiments/full_benchmark_suite/<timestamp>/
```

### CPU Investigation

Runs a targeted sweep to compare native and WAMR CPU usage with different WAMR core placement and execution modes.

```bash
bash Benchmarking_scripts/run_cpu_investigation.bash
```

or:

```bash
bash Benchmarking_scripts/run_cpu_investigation.bash /dev/cu.usbserial-XXXX
```

Output:

```text
experiments/cpu_investigation/<timestamp>/
```

### IPC Benchmarks

Runs the IPC benchmark across payload sizes and IPC mechanisms.

```bash
bash ipc_benchmark/run_all_benchmarks.bash
```

Run only selected IPC methods:

```bash
bash ipc_benchmark/run_all_benchmarks.bash 0
bash ipc_benchmark/run_all_benchmarks.bash 2 3
```

Method IDs:

- `0`, shared memory.
- `2`, stream buffer.
- `3`, queue.
- `4`, host bridge.
- `5`, native-to-Wasm invocation.

Output:

```text
experiments/ipc_benchmark/<method>/payload_<bytes>/
```

### Multi-Container Scaling Benchmark

Builds the scaling worker, packages `.wasm` and `.aot` files, flashes the scaling benchmark, and captures results.

```bash
bash ipc_benchmark/run_scaling_benchmark.bash
```

Output:

```text
experiments/ipc_benchmark/scaling/results.csv
```

Generate plots:

```bash
python3 Analysis_and_plot_scripts/plot_scaling_results.py
```

Plot output:

```text
experiments/ipc_benchmark/scaling/plots/
```

### OTA-Like Update Benchmark

Compares full flash, SPIFFS-only flash, and build-plus-flash update paths for the WAMR controller.

First make sure `Wasm_controller/build/storage.bin` exists:

```bash
cd Wasm_controller
idf.py build
```

Then run from the repository root or from `Wasm_controller`:

```bash
python3 Ota_benchmarks/ota_benchmark.py --port /dev/cu.usbserial-XXXX --runs 3
```

Output:

```text
experiments/fault_tolerance_test/ota_benchmark_results.csv
```

## Plotting and Analysis

### Plot Control Algorithm Runs

Uses the latest run under `experiments/control_algorithms/` by default.

```bash
python3 Analysis_and_plot_scripts/plot_control_algorithms.py
```

Use a specific timestamp:

```bash
python3 Analysis_and_plot_scripts/plot_control_algorithms.py --run 20260408_005413
```

Output:

```text
experiments/control_algorithms/<timestamp>/plots/
```

### Compare Native and WAMR Timing

Compares one algorithm between native and WAMR.

```bash
python3 Analysis_and_plot_scripts/compare_stats.py --algo bang_bang --mode wasm
python3 Analysis_and_plot_scripts/compare_stats.py --algo pid --mode aot
```

Output:

```text
experiments/control_algorithms/<timestamp>/plots/
```

### Plot CPU Usage

Uses the latest CPU-investigation run by default.

```bash
python3 Analysis_and_plot_scripts/graph_cpu_usage.py
```

Output:

```text
experiments/cpu_investigation/<timestamp>/plots/
```

### Analyse One CSV

Generates histograms, time series, box plots, and all-metric line graphs for a single CSV.

```bash
python3 Analysis_and_plot_scripts/analyse.py experiments/control_algorithms/<timestamp>/native/bang_bang/results.csv
```

If the input CSV is already inside `experiments/`, plots are written beside it under `graphs/`.

## Serial Capture Wrapper

Most benchmark scripts call:

```bash
python3 exec_esp32.py <output_csv> <idf.py command...>
```

The wrapper:

- Runs the supplied command, usually `idf.py build flash monitor`.
- Captures CSV sections printed between markers such as `<<<CSV_START>>>` and `<<<CSV_END>>>`.
- Captures additional sections such as runtime statistics.
- Saves watchdog and crash logs when detected.
- Terminates the monitor when runtime statistics end or a configured timeout expires.

Relative output paths are routed under:

```text
experiments/manual_runs/
```

Paths already starting with `experiments/` are kept under the requested experiment structure.

Example:

```bash
python3 exec_esp32.py experiments/manual_runs/native/results.csv idf.py build flash monitor
```

Optional timeout:

```bash
EXEC_ESP32_TIMEOUT_S=120 python3 exec_esp32.py experiments/manual_runs/results.csv idf.py build flash monitor
```

## Important Build-Time Defines

The benchmark scripts patch these `#define` values before building:

- `TEST_MODE`, selects the workload or fault experiment.
- `USE_AOT`, selects WAMR interpreter or AOT execution.
- `CONTROL_ALGORITHM_CORE`, selects the ESP32 core used by the controller workload.
- `IPC_METHOD`, selects the IPC mechanism in `ipc_benchmark`.
- `PAYLOAD_SIZE`, selects the IPC payload size.

The scripts back up and restore modified source files during each run.

## File Guide

### Repository Root

| File or directory | Purpose |
| --- | --- |
| `README.md` | This guide. |
| `exec_esp32.py` | Generic ESP32 build, flash, monitor, CSV extraction, crash-log, and watchdog-log wrapper. |
| `GEMINI.md` | Assistant-oriented project notes. Not required for running experiments. |
| `ProjectGuidelines_2024-25.pdf` | Project guidelines document. |
| `experiments/` | Experiment data, generated plots, logs, and markdown write-ups. |
| `report/` | LaTeX report source, figures, bibliography, generated PDF, and report notes. |

### Native Controller

| File | Purpose |
| --- | --- |
| `Native_controller/CMakeLists.txt` | ESP-IDF project definition for the native controller. |
| `Native_controller/main/CMakeLists.txt` | ESP-IDF component definition for native controller source files. |
| `Native_controller/main/native.c` | Native C implementation of the controller workloads, GPIO/ADC/DAC access, timing instrumentation, fault modes, hot-swap logic, CPU metrics, and CSV logging. |
| `Native_controller/sdkconfig` | ESP-IDF configuration used for native builds. |
| `Native_controller/dependencies.lock` | ESP-IDF dependency lock file. |

### Simulator

| File | Purpose |
| --- | --- |
| `Simulator/CMakeLists.txt` | ESP-IDF project definition for the simulator board. |
| `Simulator/main/CMakeLists.txt` | ESP-IDF component definition for simulator source files. |
| `Simulator/main/simulator.c` | Thermal plant simulator. Reads heater command through ADC, updates the thermal model, and outputs simulated temperature through DAC. |
| `Simulator/main/simulation_data_packet.h` | Data structure definitions used by simulator code. |
| `Simulator/main/older_ver/bridge1.c` | Older simulator or bridge prototype retained for reference. |
| `Simulator/sdkconfig` | ESP-IDF configuration used for simulator builds. |
| `Simulator/sdkconfig.old` | Previous ESP-IDF configuration snapshot. |

### WAMR Controller

| File or directory | Purpose |
| --- | --- |
| `Wasm_controller/CMakeLists.txt` | ESP-IDF project definition for the WAMR controller. It also selects which SPIFFS asset directory is packaged. |
| `Wasm_controller/main/CMakeLists.txt` | ESP-IDF component definition for the WAMR controller host. |
| `Wasm_controller/main/wasm.c` | ESP32 host firmware for WAMR. Loads Wasm or AOT modules from SPIFFS, registers host functions, runs the container in a pthread, handles metrics, faults, hot-swap behaviour, and device I/O. |
| `Wasm_controller/partitions.csv` | Flash partition layout, including the SPIFFS partition used for container binaries. |
| `Wasm_controller/sdkconfig` | ESP-IDF configuration used for WAMR builds. |
| `Wasm_controller/sdkconfig.old` | Previous ESP-IDF configuration snapshot. |
| `Wasm_controller/dependencies.lock` | ESP-IDF dependency lock file. |
| `Wasm_controller/containers/` | C source code for Wasm applications that are compiled to `.wasm` and optionally `.aot`. |
| `Wasm_controller/wasm_assets/` | Generated container binaries packaged into SPIFFS. |
| `Wasm_controller/compilers/wamrc` | WAMR AOT compiler expected by the container build script. |
| `Wasm_controller/managed_components/` | ESP-IDF managed dependency for WAMR. Generated or fetched by ESP-IDF. |

### WAMR Container Sources

| File | Purpose |
| --- | --- |
| `Wasm_controller/containers/create_container.bash` | Compiles container C files to `.wasm` using WASI SDK and to `.aot` using `wamrc` when available. |
| `Wasm_controller/containers/container_config.h` | Shared configuration used by container sources. |
| `Wasm_controller/containers/bang_bang.c` | Bang-bang control workload for WAMR. |
| `Wasm_controller/containers/bang_bang_finite.c` | Finite-duration bang-bang workload used for repeatable profiling. |
| `Wasm_controller/containers/controller_pid.c` | PID control workload for WAMR. |
| `Wasm_controller/containers/pid_finite.c` | Finite-duration PID workload used for repeatable profiling. |
| `Wasm_controller/containers/e2e_step_response.c` | Step-response workload used for end-to-end latency measurement. |
| `Wasm_controller/containers/test_end_end.c` | Earlier or alternate end-to-end test workload. |
| `Wasm_controller/containers/fault_null_ptr.c` | Fault-injection workload for null-pointer behaviour. |
| `Wasm_controller/containers/fault_overflow.c` | Fault-injection workload for stack or memory overflow behaviour. |
| `Wasm_controller/containers/fault_infinite_loop.c` | Fault-injection workload for infinite-loop/watchdog behaviour. |

### IPC and Scaling Benchmarks

| File or directory | Purpose |
| --- | --- |
| `ipc_benchmark/CMakeLists.txt` | ESP-IDF project definition for IPC and scaling benchmarks. |
| `ipc_benchmark/main/CMakeLists.txt` | ESP-IDF component definition for IPC benchmark source files. |
| `ipc_benchmark/main/ipc_benchmark.c` | Host firmware for IPC benchmark runs. Patches `IPC_METHOD` and `PAYLOAD_SIZE` to test different communication paths and payload sizes. |
| `ipc_benchmark/main/wamr_scaling_benchmark.c` | Host firmware for multi-container scaling experiments. |
| `ipc_benchmark/main/shared_data.h` | Shared message and IPC data structures. |
| `ipc_benchmark/main/scaling_worker_config.h` | Shared configuration for scaling-worker experiments. |
| `ipc_benchmark/partitions.csv` | Flash partition layout for IPC benchmark container storage. |
| `ipc_benchmark/sdkconfig` | ESP-IDF configuration used for IPC benchmark builds. |
| `ipc_benchmark/sdkconfig.defaults` | Default ESP-IDF configuration, including WAMR AOT support. |
| `ipc_benchmark/run_all_benchmarks.bash` | Automates IPC method and payload-size sweeps. Saves results under `experiments/ipc_benchmark/`. |
| `ipc_benchmark/run_scaling_benchmark.bash` | Builds the scaling worker, flashes the scaling benchmark, and saves results under `experiments/ipc_benchmark/scaling/`. |
| `ipc_benchmark/managed_components/` | ESP-IDF managed WAMR dependency. Generated or fetched by ESP-IDF. |

### IPC Container Sources

| File | Purpose |
| --- | --- |
| `ipc_benchmark/containers/create_container.bash` | Compiles IPC and scaling container sources to `.wasm` and `.aot`. |
| `ipc_benchmark/containers/producer.c` | Producer-side container for IPC benchmark methods. |
| `ipc_benchmark/containers/consumer.c` | Consumer-side container for IPC benchmark methods. |
| `ipc_benchmark/containers/native_call_producer.c` | Producer used for host-driven native-to-Wasm invocation tests. |
| `ipc_benchmark/containers/native_call_consumer.c` | Consumer used for host-driven native-to-Wasm invocation tests. |
| `ipc_benchmark/containers/scaling_worker.c` | Minimal worker container used by the multi-container scaling benchmark. |

### Benchmarking Scripts

| File | Purpose |
| --- | --- |
| `Benchmarking_scripts/run_control_algorithms.bash` | Runs bang-bang, PID, and step-response workloads across native, WAMR interpreter, and WAMR AOT. |
| `Benchmarking_scripts/run_all_controller_configs.bash` | Runs the end-to-end, hot-swap, and fault-mode configuration sweep. |
| `Benchmarking_scripts/run_full_benchmark_suite.bash` | Runs the largest controller benchmark matrix across modes, cores, and WAMR execution types. |
| `Benchmarking_scripts/run_cpu_investigation.bash` | Runs the targeted CPU-overhead investigation and writes a summary CSV. |

### Analysis and Plot Scripts

| File | Purpose |
| --- | --- |
| `Analysis_and_plot_scripts/analyse.py` | Analyses a single results CSV and generates generic timing plots. |
| `Analysis_and_plot_scripts/compare_stats.py` | Compares native and WAMR timing metrics for one control algorithm. |
| `Analysis_and_plot_scripts/graph_cpu_usage.py` | Plots native and WAMR CPU usage from CPU-investigation results. |
| `Analysis_and_plot_scripts/plot_control_algorithms.py` | Plots metric and temperature-like traces for control-algorithm runs. |
| `Analysis_and_plot_scripts/plot_scaling_results.py` | Plots heap, CPU, and startup trends from the multi-container scaling benchmark. |

### OTA Benchmarks

| File | Purpose |
| --- | --- |
| `Ota_benchmarks/ota_benchmark.py` | Measures full flash, SPIFFS-only flash, and build-plus-flash update time for the WAMR controller. |

### Experiments and Report

| File or directory | Purpose |
| --- | --- |
| `experiments/control_algorithms/` | Control-algorithm benchmark outputs. |
| `experiments/controller_config_sweep/` | Controller configuration sweep outputs. |
| `experiments/cpu_investigation/` | CPU investigation outputs. |
| `experiments/fault_tolerance_test/` | Fault, hot-swap, and OTA result data. |
| `experiments/ipc_benchmark/` | IPC and scaling benchmark outputs. |
| `experiments/*.md` | Written experiment notes and summaries. |
| `report/report.tex` | Main LaTeX report. |
| `report/references.bib` | Bibliography. |
| `report/images/` | Report figures and generated plots copied into the report. |
| `report/report.pdf` | Generated report PDF. |
| `report/SKILL.md` | Writing guidance used while editing the report. |

## Notes and Common Issues

- If `idf.py` is not found, load ESP-IDF with `source /path/to/esp-idf/export.sh` or `source "$IDF_PATH/export.sh"` if `IDF_PATH` is already set.
- If Wasm compilation fails, check that `/opt/wasi-sdk/bin/clang` exists.
- If AOT files are missing, check that `Wasm_controller/compilers/wamrc` exists and is executable.
- If flashing fails because the port is busy, close other serial monitors or pass the port explicitly.
- If Python plotting fails, install the plotting dependencies in the active Python environment.
- The benchmark scripts modify `#define` values during a run and restore the original source files on exit. Avoid manually editing the same files while a benchmark is running.
