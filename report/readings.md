# Literature Review: WebAssembly vs Native — Prior Work

Organised by theme, for use in the Background & Context chapter of the FYP report.

---

## 1. WebAssembly Performance vs Native (General)

### Not So Fast: Analyzing the Performance of WebAssembly vs. Native Code
- **Authors:** Abhinav Jangda, Bobby Powers, Emery D. Berger, Arjun Guha (UMass Amherst)
- **Venue:** USENIX ATC 2019
- **Key finding:** Wasm runs 45% slower on Firefox and 55% slower on Chrome vs native across SPEC CPU benchmarks. Debunks earlier "10% overhead" claims. Canonical reference for the Wasm-vs-native performance gap.
- **URL:** https://www.usenix.org/conference/atc19/presentation/jangda
- **arXiv:** https://arxiv.org/abs/1901.09056v3

### Understanding the Performance of WebAssembly Applications
- **Authors:** Weihang Wang, Xuan Tu et al.
- **Venue:** ACM IMC 2021
- **Key finding:** Wasm uses 3.39–4.93x more memory than JS across browsers. At small inputs Wasm is 8–27x faster than JS; at medium inputs it can be slower. Performance depends heavily on input size.
- **URL:** https://dl.acm.org/doi/10.1145/3487552.3487827
- **PDF:** https://weihang-wang.github.io/papers/imc21.pdf

### How Far We've Come – A Characterization Study of Standalone WebAssembly Runtimes
- **Authors:** Wenwen Wang
- **Venue:** IEEE IISWC 2022
- **Key finding:** Five standalone runtimes (Wasmtime, Wasmer, WAVM, Lucet, Node.js/V8) introduce 1.59–9.57x performance slowdown vs native. AOT/JIT runtimes cluster at the lower end; interpreters at the higher end.
- **URL:** https://ieeexplore.ieee.org/document/9975423/

### Characterization and Implication of Edge WebAssembly Runtimes
- **Venue:** IEEE ISPASS 2022
- **Key finding:** WAVM achieves best performance on most PolyBench cases but worst startup latency. Cranelift vs LLVM backends: LLVM produces faster code but higher startup overhead. Directly relevant to WAMR's interpreter/AOT mode trade-off.
- **IEEE:** https://ieeexplore.ieee.org/document/9781244/
- **Open access:** https://par.nsf.gov/servlets/purl/10403145

### Performance of WebAssembly Runtimes in 2023
- **Author:** Frank Denis (independent)
- **Venue:** Technical blog, January 2023
- **Key finding:** With fastest runtime, Wasm is 2.32x slower (median) than native. AES: 80x slower (no hardware AES in Wasm). Embedded Wasm interpreters still faster than MicroPython/Lua.
- **URL:** https://00f.net/2023/01/04/webassembly-benchmark-2023/

---

## 2. WebAssembly on Embedded Systems / IoT / Microcontrollers

### Benchmarking WebAssembly for Embedded Systems ⭐
- **Authors:** Konrad Moron, Stefan Wallentowitz (Munich Univ. of Applied Sciences)
- **Venue:** ACM TACO, Vol. 22, No. 3, May 2025
- **Key finding:** Evaluates WAMR interpreter and Wasm3 vs native C and MicroPython/Lua on low-resource MCUs using Embench. Wasm interpreters outperform MicroPython/Lua but are substantially slower than native. Most recent rigorous academic MCU benchmark of WAMR.
- **DOI:** https://dl.acm.org/doi/10.1145/3736169

### WebAssembly on Resource-Constrained IoT Devices ⭐
- **Authors:** Mislav Has, Tao Xiong, Fehmi Ben Abdesslem, Mario Kušek
- **Venue:** arXiv:2512.00035, December 2024
- **Key finding:** Tests WAMR and Wasm3 vs native C on **ESP32-C6, Raspberry Pi Pico, nRF5340**. Bubble sort (100 integers): native ~578 µs on ESP32-C6; Wasm3 ~6,358 µs (11x); WAMR energy 30x native. Wasm3 more memory-efficient; WAMR higher RAM requirements but richer features.
- **arXiv:** https://arxiv.org/abs/2512.00035

### Potential of WebAssembly for Embedded Systems ⭐
- **Authors:** Stefan Wallentowitz, Bastian Kersting, Dan Mihai Dumitriu
- **Venue:** IEEE MECO 2022
- **Key finding:** AOT achieves ~50% of native performance (CoreMark: 611 AOT vs 1,157 native on ARM32). Interpreter mode ~3% of native (CoreMark: 32). AOT reduces code footprint ~25% vs interpreter. Identifies 64KB minimum memory spec as key constraint.
- **DOI:** https://ieeexplore.ieee.org/document/9797106/
- **arXiv:** https://arxiv.org/abs/2405.09213

### WASMICO: Micro-containers in Microcontrollers with WebAssembly
- **Venue:** Journal of Systems and Software, Elsevier, 2024
- **Key finding:** Uses Wasm3 interpreter + FreeRTOS (same architecture as this project) for concurrent Wasm tasks on MCUs. HTTP API for remote deployment. Outperforms other MCU containerisation solutions.
- **DOI:** https://www.sciencedirect.com/science/article/abs/pii/S0164121224001262

### WARDuino: A Dynamic WebAssembly VM for Programming Microcontrollers
- **Authors:** Robbert Gurdeep Singh, Christophe Scholliers (Ghent Univ.)
- **Venue:** ACM MPLR 2019
- **Key finding:** First Wasm VM targeting Arduino-compatible MCUs including **ESP32 under ESP-IDF**. ~5x faster than Espruino (JS interpreter for ESP32). Adds live code updates and remote debugging.
- **DOI:** https://dl.acm.org/doi/abs/10.1145/3357390.3361029

### Bringing WebAssembly to Resource-Constrained IoT Devices (WAIT)
- **Authors:** Borui Li, Hongchang Fan, Yi Gao, Wei Dong
- **Venue:** ACM MobiSys 2022
- **Key finding:** WAIT AOT compiler achieves 84.8x lower RAM usage vs standard WAMR AOT. Reduces energy 1.2–4.9x. Converts structured Wasm control flow to native jump-based instructions for sub-1MB MCUs.
- **DOI:** https://dl.acm.org/doi/10.1145/3498361.3538922
- **PDF:** https://www.emnets.cn/en/publication/mobisys-22-wait/WAIT.pdf

### An Overview of WebAssembly for IoT
- **Venue:** MDPI Future Internet, Vol. 15, No. 8, August 2023
- **Key finding:** Survey of WASM-IoT ecosystem. WAMR for RTOS targets, Wasm3 for bare-metal MCUs, WasmEdge for Linux-based edge. Fast startup and near-native execution viable for IoT latency requirements.
- **URL:** https://www.mdpi.com/1999-5903/15/8/275

---

## 3. WAMR Specifically

### A Fast WebAssembly Interpreter Design in WASM-Micro-Runtime ⭐
- **Authors:** Jun Xu, Liang He, Xin Wang, Wenyong Huang, Ning Wang (Intel)
- **Venue:** Intel Technical Article, October 2021
- **Key finding:** WAMR fast interpreter achieves up to 150% performance improvement over classic stack-based interpreter. Generates only 42% of instructions while using 30% more memory. Three techniques: fast bytecode dispatching, bytecode fusion (register-based), pre-decoded LEB128. Overall 32.3% less time vs stack-based alternatives.
- **URL:** https://www.intel.com/content/www/us/en/developer/articles/technical/webassembly-interpreter-design-wasm-micro-runtime.html

### WAMR Performance Wiki
- **Venue:** Bytecode Alliance / WAMR GitHub (maintained)
- **Key finding:** Documents performance across classic interpreter, fast interpreter, AOT, JIT modes. CoreMark baselines on ARM, x86, RISC-V. AOT approaches native for compute-bound workloads.
- **URL:** https://github.com/bytecodealliance/wasm-micro-runtime/wiki/Performance

---

## 4. WebAssembly Interpreter vs AOT Compilation

### Research on WebAssembly Runtimes: A Survey
- **Authors:** Yixuan Zhang, Mugeng Liu, Haoyu Wang, Yun Ma, Gang Huang, Xuanzhe Liu (Peking Univ.)
- **Venue:** ACM TOSEM 2024; arXiv:2404.12621
- **Key finding:** 98 papers surveyed (2017–2024). AOT consistently identified as path to near-native. Interpreter: 5–10x performance loss vs native. Wasmachine (WAMR AOT) achieved 11% faster than Linux-native in some serverless scenarios.
- **arXiv:** https://arxiv.org/abs/2404.12621
- **ACM:** https://dl.acm.org/doi/10.1145/3714465

---

## 5. Real-Time Systems and WebAssembly

### Hardware-Based WebAssembly Accelerator for Embedded Systems
- **Venue:** MDPI Electronics, Vol. 13, No. 20, October 2024
- **Key finding:** FPGA-based hardware accelerator executes Wasm bytecode directly, achieving 142x speedup vs software-interpreted Wasm. Demonstrates that interpreter overhead can be eliminated by hardware decode support — context for why WAMR interpreter on ESP32 (no hardware Wasm support) shows overhead.
- **URL:** https://www.mdpi.com/2079-9292/13/20/3979

### An Evaluation of WebAssembly in Non-Web Environments
- **Authors:** Benedikt Spies, Markus U. Mock
- **Venue:** IEEE CLEI 2021
- **Key finding:** Wasm viable as lightweight alternative to native in resource-constrained environments. For IoT: portability benefits outweigh performance penalties for non-timing-critical tasks.
- **IEEE:** https://ieeexplore.ieee.org/document/9640153/

---

## 6. Edge Computing with WebAssembly

### WebAssembly for Edge Computing: Potential and Challenges
- **Authors:** M. N. Hoque, K. A. Harras
- **Venue:** IEEE Communications Standards Magazine, Vol. 6, No. 4, 2023
- **Key finding:** Wasm's portability resolves heterogeneity of edge devices. Four methods for Wasm migratability at the edge with cost trade-offs. Performance and sandboxing assessed as viable for edge offloading.
- **IEEE:** https://ieeexplore.ieee.org/iel7/7886829/10034513/10034550.pdf

### A Cross-Architecture Evaluation of WebAssembly in the Cloud-Edge Continuum
- **Venue:** IEEE CCGrid 2024
- **Key finding:** Wasmtime and Wasmer on x86-64, ARM64, RISC-V. Over 50% of benchmarks run within 2x of native with Wasmtime AOT. Validates Wasm as cross-architecture deployment substrate.
- **IEEE:** https://ieeexplore.ieee.org/document/10701368/

### On the Energy Consumption and Performance of WebAssembly in IoT
- **Authors:** Wagner, Mayer, Marino et al. (Vrije Universiteit Amsterdam)
- **Venue:** ACM EASE 2023
- **Key finding:** C and Rust are optimal source languages for IoT Wasm (validates this project's use of C). Language choice matters more than runtime choice for energy/performance. Wasmer uses 18.59% less energy than Wasmtime.
- **DOI:** https://dl.acm.org/doi/10.1145/3593434.3593454
- **PDF:** http://www.ivanomalavolta.com/files/papers/EASE_2023.pdf

### WebAssembly Beyond the Web: A Review for the Edge-Cloud Continuum
- **Venue:** IEEE CONIT 2023
- **Key finding:** Reviews Wasm as portable execution layer from browser to edge to IoT. Wasmachine (AOT) achieved 11% faster than Linux-native in edge serverless scenarios.
- **IEEE:** https://ieeexplore.ieee.org/document/10205816/

---

## Quick Reference Summary

| Paper | Venue | Year | Most Relevant Finding |
|---|---|---|---|
| Not So Fast (Jangda et al.) | USENIX ATC | 2019 | 45–55% slower than native in browser; canonical reference |
| How Far We've Come (Wang) | IEEE IISWC | 2022 | 1.59–9.57x slowdown across 5 standalone runtimes |
| Wasm Runtimes Survey (Zhang et al.) | ACM TOSEM | 2024 | 98-paper survey; AOT near-native; interpreter 5–10x overhead |
| Benchmarking Wasm for Embedded (Moron & Wallentowitz) | ACM TACO | 2025 | MCU-specific; WAMR/Wasm3 vs native on Embench |
| Wasm on IoT Devices (Has et al.) | arXiv | 2024 | **ESP32-C6**: Wasm3 11x, WAMR 30x slower than native C |
| Potential of Wasm Embedded (Wallentowitz et al.) | IEEE MECO | 2022 | AOT ≈ 50% native; interpreter ≈ 3% native (CoreMark) |
| Fast Interpreter in WAMR (Intel) | Intel Technical | 2021 | Fast interp 150% over classic; 32.3% less time vs stack-based |
| WARDuino (Singh & Scholliers) | ACM MPLR | 2019 | First Wasm VM on ESP32/ESP-IDF |
| WAIT (Li et al.) | ACM MobiSys | 2022 | 84.8x less RAM than WAMR AOT on ultra-constrained MCUs |
| Energy & Perf across Languages (Wagner et al.) | ACM EASE | 2023 | C is optimal source language for IoT Wasm |
| Hardware Wasm Accelerator | MDPI Electronics | 2024 | 142x speedup via FPGA; context for software overhead |
| WASMICO | JSS Elsevier | 2024 | FreeRTOS + Wasm on MCUs; concurrent tasks |
| Wasm for Edge (Hoque & Harras) | IEEE Comms Std Mag | 2023 | Portability enables heterogeneous edge deployment |
| Cross-Architecture Eval (CCGrid) | IEEE CCGrid | 2024 | >50% benchmarks within 2x native with AOT |

---

## Notes on Contextualising This Project's Results

- This project's **20% total overhead** (interpreter, delay-dominated control loop) and **~0% overhead** (AOT) sit at the low end of reported ranges because the workload is delay-dominated rather than compute-bound.
- Has et al. (2024) and Wallentowitz et al. (2022) report much higher overheads on compute-heavy benchmarks — the comparison is worth discussing in the report.
- The ~168 KB WAMR memory overhead measured in this project is consistent with Has et al.'s findings for WAMR on similar hardware.
- C as the source language is confirmed by Wagner et al. (2023) as the optimal choice for IoT Wasm, supporting the toolchain decision made here.
