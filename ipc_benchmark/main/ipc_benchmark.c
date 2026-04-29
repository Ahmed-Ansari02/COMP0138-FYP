/* ipc_benchmark/main/ipc_benchmark.c
 *
 * IPC Benchmark — Measures inter-container communication overhead between
 * two WASM modules on ESP32. Four methods: shared memory, stream buffer,
 * queue, and host-function bridge. Selection via #define IPC_METHOD.
 *
 * Part of UCL FYP (COMP0138) — WebAssembly on ESP32 benchmarking.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include "wasm_export.h"
#include "shared_data.h"

#define TAG "IPC_BENCH"

// ============================================================================
// CONFIGURATION
// ============================================================================

// IPC method selector: 0=shared memory, 2=stream buffer, 3=queue, 4=bridge
#define IPC_METHOD 0


// Core pinning: 0=same-core (both on core 0), 1=cross-core (producer core 0, consumer core 1)
#define CORE_MODE 0

// WASM execution mode: 0=interpreter (.wasm), 1=AOT (.aot)
#define USE_AOT 0

#if USE_AOT
#define WASM_EXT ".aot"
#else
#define WASM_EXT ".wasm"
#endif

#define NUM_ITERATIONS 10000

// ============================================================================
// TIMING MACROS
// ============================================================================

// CCOUNT cycle counter — 240MHz = 4.17ns per cycle
#define CCOUNT_READ(var) __asm__ __volatile__("rsr.ccount %0" : "=r"(var))

// ============================================================================
// RESULT TYPES
// ============================================================================

typedef struct {
    uint32_t write_cycles[NUM_ITERATIONS];
    uint32_t read_cycles[NUM_ITERATIONS];
    uint32_t round_trip_cycles[NUM_ITERATIONS];
    uint32_t free_heap_at_start;
    uint32_t free_heap_at_end;
    uint32_t iteration_count;
} ipc_results_t;

// Static results — avoids heap allocation for the large arrays
static ipc_results_t results;
static wasm_module_inst_t g_producer_inst = NULL;
static ipc_results_t *g_timing_results = NULL;
static volatile uint32_t g_timing_write_idx = 0;
static volatile uint32_t g_timing_read_idx = 0;
// ============================================================================
// WAMR MODULE LIFECYCLE
// ============================================================================

typedef struct {
    wasm_module_inst_t inst;
    wasm_module_t module;
    uint8_t *buffer;
    uint32_t buffer_size;
} wasm_loaded_module_t;

static uint8_t *load_file_from_spiffs(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buffer = malloc(size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %lu bytes for %s", (unsigned long)size, path);
        fclose(f);
        return NULL;
    }

    fread(buffer, 1, size, f);
    fclose(f);

    ESP_LOGI(TAG, "Loaded %s (%lu bytes)", path, (unsigned long)size);
    *size_out = size;
    return buffer;
}

static wasm_loaded_module_t load_module(const char *path) {
    wasm_loaded_module_t result = {0};
    char error_buf[128];

    uint32_t heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[HEAP] Before load_file_from_spiffs: %lu bytes free", (unsigned long)heap_before);

    result.buffer = load_file_from_spiffs(path, &result.buffer_size);
    if (!result.buffer) return result;

    uint32_t heap_after_file = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[HEAP] After load_file_from_spiffs:  %lu bytes free (-%lu for file buffer)",
             (unsigned long)heap_after_file, (unsigned long)(heap_before - heap_after_file));

    result.module = wasm_runtime_load(result.buffer, result.buffer_size, error_buf, sizeof(error_buf));
    if (!result.module) {
        ESP_LOGE(TAG, "WASM load failed: %s", error_buf);
        free(result.buffer);
        result.buffer = NULL;
        return result;
    }

    uint32_t heap_after_load = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[HEAP] After wasm_runtime_load:      %lu bytes free (-%lu for module parse)",
             (unsigned long)heap_after_load, (unsigned long)(heap_after_file - heap_after_load));

    result.inst = wasm_runtime_instantiate(result.module, 16 * 1024, 32 * 1024, error_buf, sizeof(error_buf));
    if (!result.inst) {
        ESP_LOGE(TAG, "WASM instantiate failed: %s", error_buf);
        wasm_runtime_unload(result.module);
        free(result.buffer);
        result.buffer = NULL;
        result.module = NULL;
        return result;
    }

    uint32_t heap_after_inst = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[HEAP] After wasm_runtime_instantiate: %lu bytes free (-%lu for instance)",
             (unsigned long)heap_after_inst, (unsigned long)(heap_after_load - heap_after_inst));
    ESP_LOGI(TAG, "[HEAP] Total consumed by %s: %lu bytes",
             path, (unsigned long)(heap_before - heap_after_inst));

    ESP_LOGI(TAG, "Module loaded and instantiated: %s", path);
    return result;
}

static wasm_exec_env_t create_exec_env(wasm_module_inst_t inst, uint32_t stack_size) {
    ESP_LOGI(TAG, "[EXEC_ENV] Requesting stack_size=%lu, free_heap=%lu, largest_block=%lu",
             (unsigned long)stack_size,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, stack_size);
    if (!env) {
        const char *exception = wasm_runtime_get_exception(inst);
        ESP_LOGE(TAG, "Failed to create exec env: %s", exception ? exception : "no exception set");
        ESP_LOGE(TAG, "[EXEC_ENV] After failure: free_heap=%lu, largest_block=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    return env;
}

static bool call_main(wasm_exec_env_t exec_env, wasm_module_inst_t inst) {
    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "main");
    if (!func) {
        ESP_LOGE(TAG, "No main() found in module");
        return false;
    }

    uint32_t args[2] = {0, 0};
    if (!wasm_runtime_call_wasm(exec_env, func, 2, args)) {
        const char *exception = wasm_runtime_get_exception(inst);
        ESP_LOGE(TAG, "WASM exception: %s", exception ? exception : "unknown");
        return false;
    }
    return true;
}

static void cleanup_module(wasm_loaded_module_t *mod) {
    if (mod->inst) {
        wasm_runtime_deinstantiate(mod->inst);
        mod->inst = NULL;
    }
    if (mod->module) {
        wasm_runtime_unload(mod->module);
        mod->module = NULL;
    }
    if (mod->buffer) {
        free(mod->buffer);
        mod->buffer = NULL;
    }
}

// ============================================================================
// METHOD 0: SHARED MEMORY — Direct Pointer Dereference
// ============================================================================
// Baseline measurement. Load a WASM module to establish linear memory,
// then do a pure C loop writing/reading SensorMessage through the raw pointer.
// No WASM execution in the timing loop — measures pointer access overhead.

#if IPC_METHOD == 0
static void run_ipc_shared_memory(ipc_results_t *results) {
    ESP_LOGI(TAG, "=== Method 0: Shared Memory (Direct Pointer) ===");

    wasm_loaded_module_t prod = load_module("/spiffs/producer" WASM_EXT);
    if (!prod.inst) {
        ESP_LOGE(TAG, "Failed to load producer module");
        return;
    }

    // Get pointer to offset 0 of producer's linear memory
    struct SensorMessage *shared_msg =
        (struct SensorMessage *)wasm_runtime_addr_app_to_native(prod.inst, 0);
    if (!shared_msg) {
        ESP_LOGE(TAG, "Failed to get native address for WASM memory");
        cleanup_module(&prod);
        return;
    }

    ESP_LOGI(TAG, "Shared pointer: %p (SensorMessage, %u bytes), running %d iterations",
             shared_msg, (unsigned)sizeof(struct SensorMessage), NUM_ITERATIONS);

    results->free_heap_at_start = esp_get_free_heap_size();

    struct SensorMessage src = {0};
    src.status = 1;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t c0, c1, c2;

        src.timestamp_ms = (uint32_t)(i * 10);
        src.sensor_id = (uint16_t)(i & 0xFFFF);
        src.value = (int16_t)(i % 100);
        src.priority = (uint8_t)(i & 0x03);

        CCOUNT_READ(c0);
        *shared_msg = src;
        CCOUNT_READ(c1);
        volatile struct SensorMessage dst = *shared_msg;
        (void)dst;
        CCOUNT_READ(c2);

        results->write_cycles[i] = c1 - c0;
        results->read_cycles[i] = c2 - c1;
        results->round_trip_cycles[i] = c2 - c0;
    }

    results->iteration_count = NUM_ITERATIONS;
    results->free_heap_at_end = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Shared memory benchmark complete");
    cleanup_module(&prod);
}
#endif // IPC_METHOD == 0

// ============================================================================
// METHOD 4: HOST-FUNCTION BRIDGE — Embedder-Mediated Cross-Module Read
// ============================================================================
// Two WASM module instances loaded. Producer writes SensorMessage to its linear
// memory via a host function. Consumer reads from producer's memory via a
// separate host function using wasm_runtime_addr_app_to_native() on the
// producer's module instance. Both run sequentially in the same task.

#if IPC_METHOD == 4


#define BRIDGE_DATA_OFFSET 0
static struct SensorMessage g_shared_msg; // shared struct for producer→host communication
// Called by producer WASM: copies SensorMessage to producer's linear memory at offset 0
static void host_ipc_write(wasm_exec_env_t exec_env, uint32_t msg_ptr) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);

    struct SensorMessage *src =
        (struct SensorMessage *)wasm_runtime_addr_app_to_native(inst, msg_ptr);
    if (!src) return;

    g_shared_msg = *src;
}

// Called by consumer WASM: reads SensorMessage from producer's linear memory (cross-module)
static void host_ipc_bridge_read(wasm_exec_env_t exec_env, uint32_t buf_ptr) {
    wasm_module_inst_t cons_inst = wasm_runtime_get_module_inst(exec_env);

    struct SensorMessage *dst =
        (struct SensorMessage *)wasm_runtime_addr_app_to_native(cons_inst, buf_ptr);
    if (!dst) return;

    *dst = g_shared_msg;
}

static void run_ipc_bridge(ipc_results_t *results) {
    ESP_LOGI(TAG, "=== Method 4: Host-Function Bridge (Cross-Module Read) ===");

    wasm_loaded_module_t prod = load_module("/spiffs/producer" WASM_EXT);
    if (!prod.inst) {
        ESP_LOGE(TAG, "Failed to load producer module");
        return;
    }

    ESP_LOGI(TAG, "Free heap after producer load: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Largest free block: %lu bytes", (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    wasm_loaded_module_t cons = load_module("/spiffs/consumer" WASM_EXT);
    if (!cons.inst) {
        ESP_LOGE(TAG, "Failed to load consumer module");
        cleanup_module(&prod);
        return;
    }

    g_producer_inst = prod.inst;
    g_timing_results = results;
    g_timing_write_idx = 0;
    g_timing_read_idx = 0;

    results->free_heap_at_start = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Running producer (writes %d values)...", NUM_ITERATIONS);
    wasm_exec_env_t prod_env = create_exec_env(prod.inst, 8 * 1024);
    if (prod_env) {
        call_main(prod_env, prod.inst);
        wasm_runtime_destroy_exec_env(prod_env);
    }

    ESP_LOGI(TAG, "Running consumer (reads %d values via bridge)...", NUM_ITERATIONS);
    wasm_exec_env_t cons_env = create_exec_env(cons.inst, 8 * 1024);
    if (cons_env) {
        call_main(cons_env, cons.inst);
        wasm_runtime_destroy_exec_env(cons_env);
    }

    results->iteration_count = g_timing_read_idx;
    results->free_heap_at_end = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Bridge benchmark complete (%lu iterations)", (unsigned long)results->iteration_count);

    g_producer_inst = NULL;
    g_timing_results = NULL;
    cleanup_module(&cons);
    cleanup_module(&prod);
}
#endif // IPC_METHOD == 4

// ============================================================================
// METHOD 5: NATIVE→WASM BRIDGE — Host calls exported WASM functions
// ============================================================================
// Measures the cost of calling into WASM from the host side (opposite direction
// to Method 4). Host loop calls producer's exported produce(i), reads data from
// producer's linear memory, writes to consumer's, calls consumer's consume().

#if IPC_METHOD == 5

#define NATIVE_CALL_DATA_OFFSET 0
static struct SensorMessage g_shared_msg; // shared struct for producer→host communication
static void run_ipc_native_call(ipc_results_t *results) {
    ESP_LOGI(TAG, "=== Method 5: Native->Wasm Bridge (core_mode=%d) ===", CORE_MODE);

    wasm_loaded_module_t prod = load_module("/spiffs/native_call_producer" WASM_EXT);
    if (!prod.inst) { ESP_LOGE(TAG, "Failed to load native_call_producer"); return; }

    wasm_loaded_module_t cons = load_module("/spiffs/native_call_consumer" WASM_EXT);
    if (!cons.inst) { ESP_LOGE(TAG, "Failed to load native_call_consumer"); cleanup_module(&prod); return; }

    // Look up exported functions
    wasm_function_inst_t produce_func = wasm_runtime_lookup_function(prod.inst, "produce");
    wasm_function_inst_t consume_func = wasm_runtime_lookup_function(cons.inst, "consume");
    if (!produce_func || !consume_func) {
        ESP_LOGE(TAG, "Failed to find exported functions (produce=%p, consume=%p)",
                 produce_func, consume_func);
        cleanup_module(&cons);
        cleanup_module(&prod);
        return;
    }

    // Create exec envs for calling into WASM
    wasm_exec_env_t prod_env = create_exec_env(prod.inst, 8 * 1024);
    wasm_exec_env_t cons_env = create_exec_env(cons.inst, 8 * 1024);
    if (!prod_env || !cons_env) {
        if (prod_env) wasm_runtime_destroy_exec_env(prod_env);
        if (cons_env) wasm_runtime_destroy_exec_env(cons_env);
        cleanup_module(&cons);
        cleanup_module(&prod);
        return;
    }

    results->free_heap_at_start = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Running %d iterations (host-driven loop)...", NUM_ITERATIONS);

    void *consumer_msg_native = NULL;
    uint64_t consumer_msg_offset64 =
        wasm_runtime_module_malloc(cons.inst, sizeof(struct SensorMessage), &consumer_msg_native);
    if (!consumer_msg_offset64 || !consumer_msg_native) {
        ESP_LOGE(TAG, "Failed to allocate consumer message buffer");
        wasm_runtime_destroy_exec_env(prod_env);
        wasm_runtime_destroy_exec_env(cons_env);
        cleanup_module(&cons);
        cleanup_module(&prod);
        return;
    }
    uint32_t consumer_msg_offset = (uint32_t)consumer_msg_offset64;

    for (uint32_t i = 0; i < NUM_ITERATIONS; i++) {
        uint32_t c0, c1, c2, c3;

        // 1) Host calls producer's exported produce(i) — native→Wasm
        uint32_t prod_args[2];
        CCOUNT_READ(c0);
        wasm_runtime_call_wasm(prod_env, produce_func, 1, prod_args);
        g_shared_msg = *((struct SensorMessage *)wasm_runtime_addr_app_to_native(prod.inst, prod_args[0])); 
        CCOUNT_READ(c1);

        // 2) Copy g_shared_msg to consumer WASM memory and call consume(msg_ptr)
        CCOUNT_READ(c2);
        *((struct SensorMessage *)consumer_msg_native) = g_shared_msg;
        uint32_t cons_args[1] = { consumer_msg_offset };
        wasm_runtime_call_wasm(cons_env, consume_func, 1, cons_args);
        CCOUNT_READ(c3);

        results->write_cycles[i] = c1 - c0;      // produce() call cost
        results->read_cycles[i] = c3 - c2;        // consume() call cost
        results->round_trip_cycles[i] = c3 - c0;  // total round-trip

        // Let IDLE task run periodically so task watchdog doesn't fire.
        // This is outside measured timing windows (after c3).
        if ((i & 0x7F) == 0x7F) {
            vTaskDelay(1);
        }
    }

    results->iteration_count = NUM_ITERATIONS;
    results->free_heap_at_end = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Native->Wasm benchmark complete (%lu iterations)",
             (unsigned long)results->iteration_count);

    wasm_runtime_module_free(cons.inst, consumer_msg_offset64);
    wasm_runtime_destroy_exec_env(prod_env);
    wasm_runtime_destroy_exec_env(cons_env);
    cleanup_module(&cons);
    cleanup_module(&prod);
}
#endif // IPC_METHOD == 5

// ============================================================================
// CONCURRENT IPC — Shared state and task helpers for Methods 2 & 3
// ============================================================================

#if IPC_METHOD == 2 || IPC_METHOD == 3

#if IPC_METHOD == 2
static StreamBufferHandle_t g_stream_buf = NULL;
#elif IPC_METHOD == 3
static QueueHandle_t g_queue = NULL;
#endif

// ---- HOST FUNCTIONS ----

static void host_ipc_send(wasm_exec_env_t exec_env, uint32_t msg_ptr) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    struct SensorMessage *msg =
        (struct SensorMessage *)wasm_runtime_addr_app_to_native(inst, msg_ptr);
    if (!msg) return;

#if IPC_METHOD == 2
    xStreamBufferSend(g_stream_buf, msg, sizeof(struct SensorMessage), portMAX_DELAY);
#elif IPC_METHOD == 3
    xQueueSend(g_queue, msg, portMAX_DELAY);
#endif
}

static void host_ipc_recv(wasm_exec_env_t exec_env, uint32_t buf_ptr) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    struct SensorMessage *buf =
        (struct SensorMessage *)wasm_runtime_addr_app_to_native(inst, buf_ptr);
    if (!buf) return;

    struct SensorMessage tmp;
#if IPC_METHOD == 2
    xStreamBufferReceive(g_stream_buf, &tmp, sizeof(struct SensorMessage), portMAX_DELAY);
#elif IPC_METHOD == 3
    xQueueReceive(g_queue, &tmp, portMAX_DELAY);
#endif
    *buf = tmp;
}

// ---- TASKS (pthreads — WAMR requires pthread context) ----

typedef struct {
    wasm_module_inst_t inst;
} task_arg_t;

static void *producer_task(void *arg) {
    task_arg_t *targ = (task_arg_t *)arg;
    wasm_exec_env_t env = create_exec_env(targ->inst, 8 * 1024);
    if (env) {
        call_main(env, targ->inst);
        wasm_runtime_destroy_exec_env(env);
    }
    ESP_LOGI(TAG, "Producer task done (%lu sends)", (unsigned long)g_timing_write_idx);
    return NULL;
}

static void *consumer_task(void *arg) {
    task_arg_t *targ = (task_arg_t *)arg;
    wasm_exec_env_t env = create_exec_env(targ->inst, 8 * 1024);
    if (env) {
        call_main(env, targ->inst);
        wasm_runtime_destroy_exec_env(env);
    }
    ESP_LOGI(TAG, "Consumer task done (%lu recvs)", (unsigned long)g_timing_read_idx);
    return NULL;
}

// Helper: create a pthread pinned to a specific core
static int create_pinned_pthread(pthread_t *thread, void *(*func)(void *),
                                  void *arg, int core_id, uint32_t stack_size) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = stack_size;
    cfg.prio = 5;
    cfg.pin_to_core = core_id;
    esp_pthread_set_cfg(&cfg);
    return pthread_create(thread, NULL, func, arg);
}

// ---- BENCHMARK ENTRY POINTS ----

#if IPC_METHOD == 2
static void run_ipc_stream_buffer(ipc_results_t *results) {
    ESP_LOGI(TAG, "=== Method 2: Stream Buffer (core_mode=%d) ===", CORE_MODE);

    wasm_loaded_module_t prod = load_module("/spiffs/producer" WASM_EXT);
    if (!prod.inst) { ESP_LOGE(TAG, "Failed to load producer"); return; }

    wasm_loaded_module_t cons = load_module("/spiffs/consumer" WASM_EXT);
    if (!cons.inst) { ESP_LOGE(TAG, "Failed to load consumer"); cleanup_module(&prod); return; }

    // Buffer must hold at least a few messages; scale with struct size
    size_t sb_size = sizeof(struct SensorMessage) * 8;
    if (sb_size < 128) sb_size = 128;
    g_stream_buf = xStreamBufferCreate(sb_size, sizeof(struct SensorMessage));
    g_timing_results = results;
    g_timing_write_idx = 0;
    g_timing_read_idx = 0;

    results->free_heap_at_start = esp_get_free_heap_size();

    static task_arg_t prod_arg, cons_arg;
    prod_arg.inst = prod.inst;
    cons_arg.inst = cons.inst;

    int core_cons = (CORE_MODE == 1) ? 1 : 0;

    pthread_t cons_thread, prod_thread;
    create_pinned_pthread(&cons_thread, consumer_task, &cons_arg, core_cons, 12 * 1024);
    vTaskDelay(pdMS_TO_TICKS(10));
    create_pinned_pthread(&prod_thread, producer_task, &prod_arg, 0, 12 * 1024);

    pthread_join(cons_thread, NULL);
    pthread_join(prod_thread, NULL);

    results->iteration_count = (g_timing_read_idx < NUM_ITERATIONS) ? g_timing_read_idx : NUM_ITERATIONS;
    results->free_heap_at_end = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Stream buffer benchmark complete (%lu iterations)", (unsigned long)results->iteration_count);

    vStreamBufferDelete(g_stream_buf);
    g_stream_buf = NULL;
    g_timing_results = NULL;

    cleanup_module(&cons);
    cleanup_module(&prod);
}
#endif // IPC_METHOD == 2

#if IPC_METHOD == 3
static void run_ipc_queue(ipc_results_t *results) {
    ESP_LOGI(TAG, "=== Method 3: Queue (core_mode=%d) ===", CORE_MODE);

    wasm_loaded_module_t prod = load_module("/spiffs/producer" WASM_EXT);
    if (!prod.inst) { ESP_LOGE(TAG, "Failed to load producer"); return; }

    wasm_loaded_module_t cons = load_module("/spiffs/consumer" WASM_EXT);
    if (!cons.inst) { ESP_LOGE(TAG, "Failed to load consumer"); cleanup_module(&prod); return; }

    g_queue = xQueueCreate(16, sizeof(struct SensorMessage));
    g_timing_results = results;
    g_timing_write_idx = 0;
    g_timing_read_idx = 0;

    results->free_heap_at_start = esp_get_free_heap_size();

    static task_arg_t prod_arg, cons_arg;
    prod_arg.inst = prod.inst;
    cons_arg.inst = cons.inst;

    int core_cons = (CORE_MODE == 1) ? 1 : 0;

    pthread_t cons_thread, prod_thread;
    create_pinned_pthread(&cons_thread, consumer_task, &cons_arg, core_cons, 12 * 1024);
    vTaskDelay(pdMS_TO_TICKS(10));
    create_pinned_pthread(&prod_thread, producer_task, &prod_arg, 0, 12 * 1024);

    pthread_join(cons_thread, NULL);
    pthread_join(prod_thread, NULL);

    results->iteration_count = (g_timing_read_idx < NUM_ITERATIONS) ? g_timing_read_idx : NUM_ITERATIONS;
    results->free_heap_at_end = esp_get_free_heap_size();

    ESP_LOGI(TAG, "Queue benchmark complete (%lu iterations)", (unsigned long)results->iteration_count);

    vQueueDelete(g_queue);
    g_queue = NULL;
    g_timing_results = NULL;

    cleanup_module(&cons);
    cleanup_module(&prod);
}
#endif // IPC_METHOD == 3

#endif // IPC_METHOD == 2 || IPC_METHOD == 3

// ============================================================================
// TIMING HOST FUNCTIONS — Called by WASM containers to self-measure
// ============================================================================


static uint32_t host_get_cycles(wasm_exec_env_t exec_env) {
    uint32_t c;
    CCOUNT_READ(c);
    return c;
}

static void host_record_write_time(wasm_exec_env_t exec_env, uint32_t cycles) {
    uint32_t idx = g_timing_write_idx;
    if (g_timing_results && idx < NUM_ITERATIONS) {
        g_timing_results->write_cycles[idx] = cycles;
    }
    g_timing_write_idx = idx + 1;
}

static void host_record_read_time(wasm_exec_env_t exec_env, uint32_t cycles) {
    uint32_t idx = g_timing_read_idx;
    if (g_timing_results && idx < NUM_ITERATIONS) {
        g_timing_results->read_cycles[idx] = cycles;
        g_timing_results->round_trip_cycles[idx] =
            g_timing_results->write_cycles[idx] + cycles;
    }
    g_timing_read_idx = idx + 1;
}

// ============================================================================
// NATIVE SYMBOL REGISTRATION
// ============================================================================

#if IPC_METHOD == 2 || IPC_METHOD == 3
static NativeSymbol native_symbols[] = {
    {"ipc_send", host_ipc_send, "(i)", NULL},
    {"ipc_recv", host_ipc_recv, "(i)", NULL},
    {"host_get_cycles", host_get_cycles, "()i", NULL},
    {"host_record_write_time", host_record_write_time, "(i)", NULL},
    {"host_record_read_time", host_record_read_time, "(i)", NULL},
};
#elif IPC_METHOD == 4
static NativeSymbol native_symbols[] = {
    {"ipc_send", host_ipc_write, "(i)", NULL},
    {"ipc_recv", host_ipc_bridge_read, "(i)", NULL},
    {"host_get_cycles", host_get_cycles, "()i", NULL},
    {"host_record_write_time", host_record_write_time, "(i)", NULL},
    {"host_record_read_time", host_record_read_time, "(i)", NULL},
};
#else
static NativeSymbol native_symbols[] = {
    {"host_get_cycles", host_get_cycles, "()i", NULL},
    {"host_record_write_time", host_record_write_time, "(i)", NULL},
    {"host_record_read_time", host_record_read_time, "(i)", NULL},
};
#endif

// ============================================================================
// CSV OUTPUT
// ============================================================================

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void print_ipc_csv(const ipc_results_t *results, const char *method_name) {
    uint32_t count = results->iteration_count;
    if (count > NUM_ITERATIONS) count = NUM_ITERATIONS;

    vTaskDelay(pdMS_TO_TICKS(100));

    printf("\n<<<CSV_START>>>\n");
    printf("method,iteration,write_cycles,read_cycles,round_trip_cycles,free_heap\n");

    for (uint32_t i = 0; i < count; i++) {
        printf("%s,%lu,%lu,%lu,%lu,%lu\n",
               method_name,
               (unsigned long)i,
               (unsigned long)results->write_cycles[i],
               (unsigned long)results->read_cycles[i],
               (unsigned long)results->round_trip_cycles[i],
               (unsigned long)results->free_heap_at_start);

        // Yield periodically to feed the task watchdog during large CSV output
        if ((i % 1000) == 999) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    printf("<<<CSV_END>>>\n\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void print_summary_stats(const ipc_results_t *results, const char *method_name) {
    uint32_t count = results->iteration_count;
    if (count == 0) return;
    if (count > NUM_ITERATIONS) count = NUM_ITERATIONS;

    uint32_t *sorted = malloc(count * sizeof(uint32_t));
    if (!sorted) {
        ESP_LOGE(TAG, "Failed to allocate memory for summary stats");
        return;
    }
    memcpy(sorted, results->round_trip_cycles, count * sizeof(uint32_t));
    qsort(sorted, count, sizeof(uint32_t), cmp_u32);

    uint32_t min_val = sorted[0];
    uint32_t max_val = sorted[count - 1];
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        sum += sorted[i];
    }
    double mean_val = (double)sum / count;
    uint32_t p50 = sorted[count / 2];
    uint32_t p95 = sorted[(uint32_t)((count - 1) * 0.95)];
    uint32_t p99 = sorted[(uint32_t)((count - 1) * 0.99)];

    free(sorted);

    const char *core_mode_str = (CORE_MODE == 0) ? "same_core" : "cross_core";

    printf("\n<<<SUMMARY_START>>>\n");
    printf("method,metric,value\n");
    printf("%s,min_cycles,%lu\n", method_name, (unsigned long)min_val);
    printf("%s,max_cycles,%lu\n", method_name, (unsigned long)max_val);
    printf("%s,mean_cycles,%.1f\n", method_name, mean_val);
    printf("%s,p50_cycles,%lu\n", method_name, (unsigned long)p50);
    printf("%s,p95_cycles,%lu\n", method_name, (unsigned long)p95);
    printf("%s,p99_cycles,%lu\n", method_name, (unsigned long)p99);
    printf("%s,free_heap_start,%lu\n", method_name, (unsigned long)results->free_heap_at_start);
    printf("%s,free_heap_end,%lu\n", method_name, (unsigned long)results->free_heap_at_end);
    printf("%s,core_mode,%s\n", method_name, core_mode_str);
    printf("%s,iterations,%lu\n", method_name, (unsigned long)count);
    printf("<<<SUMMARY_END>>>\n\n");
    fflush(stdout);
}

static void print_task_runtime_stats(void) {
    char *stats_buffer = malloc(2048);
    if (!stats_buffer) {
        ESP_LOGE(TAG, "Failed to allocate stats buffer");
        return;
    }

    vTaskGetRunTimeStats(stats_buffer);

    printf("\n<<<RUNTIME_STATS_START>>>\n");
    printf("Task            Abs Time      %% Time\n");
    printf("%s", stats_buffer);
    printf("<<<RUNTIME_STATS_END>>>\n\n");
    fflush(stdout);

    free(stats_buffer);
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ============================================================================
// WAMR INIT / SHUTDOWN
// ============================================================================

static void wasm_host_init(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed");
        return;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", total, used);

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "WAMR init failed");
        return;
    }

    uint32_t num_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    if (num_symbols > 0) {
        wasm_runtime_register_natives("env", native_symbols, num_symbols);
    }

    ESP_LOGI(TAG, "WAMR initialized, %lu native symbols registered", (unsigned long)num_symbols);
}

static void wasm_host_shutdown(void) {
    wasm_runtime_destroy();
    esp_vfs_spiffs_unregister("storage");
    ESP_LOGI(TAG, "WAMR shutdown complete");
}

// ============================================================================
// BENCHMARK THREAD (WAMR requires pthread context)
// ============================================================================

static void *benchmark_thread_entry(void *arg) {
    wasm_host_init();

    memset(&results, 0, sizeof(results));

#if IPC_METHOD == 0
    const char *method_name = "shared_memory";
    run_ipc_shared_memory(&results);
#elif IPC_METHOD == 2
    const char *method_name = "stream_buffer";
    run_ipc_stream_buffer(&results);
#elif IPC_METHOD == 3
    const char *method_name = "queue";
    run_ipc_queue(&results);
#elif IPC_METHOD == 4
    const char *method_name = "host_bridge";
    run_ipc_bridge(&results);
#elif IPC_METHOD == 5
    const char *method_name = "native_to_wasm";
    run_ipc_native_call(&results);
#else
    #error "Invalid IPC_METHOD. Use 0-5 (0=shared_memory, 2=stream_buffer, 3=queue, 4=bridge, 5=native_to_wasm)."
#endif

    print_ipc_csv(&results, method_name);
    print_summary_stats(&results, method_name);
    print_task_runtime_stats();

    wasm_host_shutdown();

    ESP_LOGI(TAG, "Benchmark complete. Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    return NULL;
}

// ============================================================================
// APP_MAIN
// ============================================================================

void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "IPC Benchmark — Method %d, Core Mode %d", IPC_METHOD, CORE_MODE);
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Free heap at start: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // WAMR requires pthread context — run benchmark in a pthread (same as controller_wamr.c)
    esp_pthread_cfg_t pthread_cfg = esp_pthread_get_default_config();
    pthread_cfg.stack_size = 24 * 1024;
    pthread_cfg.prio = 5;
    esp_pthread_set_cfg(&pthread_cfg);

    pthread_t bench_thread;
    int res = pthread_create(&bench_thread, NULL, benchmark_thread_entry, NULL);
    if (res != 0) {
        ESP_LOGE(TAG, "Failed to create benchmark thread: %d", res);
        return;
    }

    pthread_join(bench_thread, NULL);
}
