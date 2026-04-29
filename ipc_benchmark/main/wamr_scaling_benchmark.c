/* ipc_benchmark/main/wamr_scaling_benchmark.c
 *
 * Low-memory multi-container WAMR scaling benchmark for ESP32.
 * Sweeps the number of concurrently active worker containers upward until the
 * first failed or unstable container count is observed.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scaling_worker_config.h"
#include "wasm_export.h"

#define TAG "WAMR_SCALE"

#define SCALING_MAX_CONTAINERS 24
#define SCALING_MAX_TASKS 32
#define SCALING_PREPARE_TIMEOUT_MS 3000
#define SCALING_RUN_TIMEOUT_MS 15000
#define SCALING_CPU_SAMPLE_INTERVAL_MS 100
#define SCALING_MONITOR_POLL_MS 20

#define SCALING_INSTANCE_STACK_BYTES 0
#define SCALING_INSTANCE_HEAP_BYTES 0
#define SCALING_EXEC_ENV_STACK_BYTES (2 * 1024)
#define SCALING_PTHREAD_STACK_BYTES (6 * 1024)
#define SCALING_BENCHMARK_THREAD_STACK_BYTES (24 * 1024)

typedef enum {
    SCALE_STATUS_OK = 0,
    SCALE_STATUS_FILE_LOAD_FAILED,
    SCALE_STATUS_MODULE_LOAD_FAILED,
    SCALE_STATUS_INSTANTIATE_FAILED,
    SCALE_STATUS_EXEC_ENV_FAILED,
    SCALE_STATUS_PTHREAD_FAILED,
    SCALE_STATUS_TIMEOUT,
    SCALE_STATUS_WASM_EXCEPTION,
    SCALE_STATUS_RESULT_MISMATCH,
    SCALE_STATUS_INCOMPLETE,
} scaling_status_t;

typedef struct {
    const char *mode_name;
    const char *module_path;
} scaling_mode_config_t;

typedef struct {
    uint8_t *buffer;
    uint32_t buffer_size;
    wasm_module_t module;
    wasm_module_inst_t inst;
    wasm_exec_env_t exec_env;
    pthread_t thread;
    bool thread_created;
    bool prepared;
    bool started;
    bool call_completed;
    bool call_ok;
    bool checksum_reported;
    uint32_t checksum;
    int64_t first_exec_latency_us;
    uint64_t load_time_us;
    uint64_t instantiate_time_us;
    char failure_reason[128];
} scaling_container_t;

typedef struct scaling_run scaling_run_t;

typedef struct {
    scaling_run_t *run;
    uint32_t container_index;
} worker_thread_arg_t;

struct scaling_run {
    scaling_container_t containers[SCALING_MAX_CONTAINERS];
    worker_thread_arg_t thread_args[SCALING_MAX_CONTAINERS];
    uint32_t container_count;
    uint32_t created_threads;
    uint32_t prepared_count;
    uint32_t ready_count;
    uint32_t completed_count;
    bool launch_failed;
    bool start_released;
    bool abort_start;
    bool timed_out;
    int64_t start_release_us;
    pthread_mutex_t state_lock;
    pthread_cond_t start_cond;
};

typedef struct {
    const char *mode;
    uint32_t container_count;
    scaling_status_t status;
    uint64_t load_time_total_us;
    uint64_t load_time_mean_us;
    uint64_t instantiate_time_total_us;
    uint64_t instantiate_time_mean_us;
    uint64_t startup_time_total_us;
    uint64_t first_exec_latency_mean_us;
    uint64_t first_exec_latency_max_us;
    uint32_t free_heap_before;
    uint32_t free_heap_after_init;
    uint32_t free_heap_after_load;
    uint32_t free_heap_after_instantiate;
    uint32_t free_heap_steady;
    float cpu_core0_mean;
    float cpu_core1_mean;
    float cpu_overall_mean;
    uint32_t highest_successful_n;
} scaling_csv_row_t;

typedef struct {
    const char *mode;
    uint32_t highest_successful_n;
    scaling_status_t terminal_status;
} scaling_mode_summary_t;

typedef struct {
    double sum_core0;
    double sum_core1;
    double sum_overall;
    uint32_t sample_count;
} cpu_mean_accumulator_t;

static const scaling_mode_config_t scaling_modes[] = {
    {"interpreter", "/spiffs/scaling_worker.wasm"},
    {"aot", "/spiffs/scaling_worker.aot"},
};

static scaling_csv_row_t g_rows[SCALING_MAX_CONTAINERS
                                * (sizeof(scaling_modes)
                                   / sizeof(scaling_modes[0]))];
static size_t g_row_count = 0;
static scaling_mode_summary_t g_mode_summaries[sizeof(scaling_modes)
                                               / sizeof(scaling_modes[0])];

static scaling_run_t *g_active_run = NULL;
static TaskStatus_t g_task_status_array[SCALING_MAX_TASKS];
static uint32_t g_prev_total_runtime = 0;
static uint32_t g_free_heap_before_init = 0;
static uint32_t g_free_heap_after_init = 0;

static const char *
status_to_string(scaling_status_t status)
{
    switch (status) {
    case SCALE_STATUS_OK:
        return "ok";
    case SCALE_STATUS_FILE_LOAD_FAILED:
        return "file_load_failed";
    case SCALE_STATUS_MODULE_LOAD_FAILED:
        return "module_load_failed";
    case SCALE_STATUS_INSTANTIATE_FAILED:
        return "instantiate_failed";
    case SCALE_STATUS_EXEC_ENV_FAILED:
        return "exec_env_failed";
    case SCALE_STATUS_PTHREAD_FAILED:
        return "pthread_failed";
    case SCALE_STATUS_TIMEOUT:
        return "timeout";
    case SCALE_STATUS_WASM_EXCEPTION:
        return "wasm_exception";
    case SCALE_STATUS_RESULT_MISMATCH:
        return "result_mismatch";
    case SCALE_STATUS_INCOMPLETE:
        return "incomplete";
    default:
        return "unknown";
    }
}

static void
copy_reason(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src || src[0] == '\0') {
        src = "unspecified";
    }

    snprintf(dst, dst_size, "%s", src);
}

static int
find_container_index(const scaling_run_t *run, wasm_module_inst_t inst)
{
    if (!run || !inst) {
        return -1;
    }

    for (uint32_t i = 0; i < run->container_count; ++i) {
        if (run->containers[i].inst == inst) {
            return (int)i;
        }
    }

    return -1;
}

static void
host_worker_started(wasm_exec_env_t exec_env)
{
    scaling_run_t *run = g_active_run;
    wasm_module_inst_t inst;
    int idx;
    int64_t now_us;

    if (!run) {
        return;
    }

    inst = wasm_runtime_get_module_inst(exec_env);
    idx = find_container_index(run, inst);
    if (idx < 0) {
        return;
    }

    now_us = esp_timer_get_time();

    pthread_mutex_lock(&run->state_lock);
    if (!run->containers[idx].started) {
        run->containers[idx].started = true;
        run->containers[idx].first_exec_latency_us = now_us - run->start_release_us;
    }
    pthread_mutex_unlock(&run->state_lock);
}

static void
host_report_checksum(wasm_exec_env_t exec_env, uint32_t checksum)
{
    scaling_run_t *run = g_active_run;
    wasm_module_inst_t inst;
    int idx;

    if (!run) {
        return;
    }

    inst = wasm_runtime_get_module_inst(exec_env);
    idx = find_container_index(run, inst);
    if (idx < 0) {
        return;
    }

    pthread_mutex_lock(&run->state_lock);
    run->containers[idx].checksum = checksum;
    run->containers[idx].checksum_reported = true;
    pthread_mutex_unlock(&run->state_lock);
}

static void
host_delay(wasm_exec_env_t exec_env, uint32_t delay_ms)
{
    (void)exec_env;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static void
host_cooperate(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    vTaskDelay(1);
}

static NativeSymbol native_symbols[] = {
    {"host_worker_started", host_worker_started, "()", NULL},
    {"host_report_checksum", host_report_checksum, "(i)", NULL},
    {"host_delay", host_delay, "(i)", NULL},
    {"host_cooperate", host_cooperate, "()", NULL},
};

static uint8_t *
load_file_from_spiffs(const char *path, uint32_t *size_out)
{
    FILE *f;
    uint8_t *buffer;
    uint32_t size;

    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    size = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    buffer = malloc(size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %lu bytes for %s",
                 (unsigned long)size, path);
        fclose(f);
        return NULL;
    }

    if (fread(buffer, 1, size, f) != size) {
        ESP_LOGE(TAG, "Failed to read %s", path);
        free(buffer);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = size;
    return buffer;
}

static scaling_status_t
load_container_module(const char *path, scaling_container_t *container)
{
    char error_buf[128];
    int64_t start_us;

    memset(error_buf, 0, sizeof(error_buf));

    start_us = esp_timer_get_time();
    container->buffer = load_file_from_spiffs(path, &container->buffer_size);
    if (!container->buffer) {
        copy_reason(container->failure_reason, sizeof(container->failure_reason),
                    "SPIFFS read failed");
        return SCALE_STATUS_FILE_LOAD_FAILED;
    }

    container->module = wasm_runtime_load(container->buffer, container->buffer_size,
                                          error_buf, sizeof(error_buf));
    container->load_time_us = (uint64_t)(esp_timer_get_time() - start_us);

    if (!container->module) {
        copy_reason(container->failure_reason, sizeof(container->failure_reason),
                    error_buf);
        return SCALE_STATUS_MODULE_LOAD_FAILED;
    }

    return SCALE_STATUS_OK;
}

static scaling_status_t
instantiate_container(scaling_container_t *container)
{
    char error_buf[128];
    int64_t start_us;

    memset(error_buf, 0, sizeof(error_buf));
    start_us = esp_timer_get_time();

    container->inst = wasm_runtime_instantiate(container->module,
                                               SCALING_INSTANCE_STACK_BYTES,
                                               SCALING_INSTANCE_HEAP_BYTES,
                                               error_buf,
                                               sizeof(error_buf));
    container->instantiate_time_us = (uint64_t)(esp_timer_get_time() - start_us);

    if (!container->inst) {
        copy_reason(container->failure_reason, sizeof(container->failure_reason),
                    error_buf);
        return SCALE_STATUS_INSTANTIATE_FAILED;
    }

    return SCALE_STATUS_OK;
}

static void
cleanup_container(scaling_container_t *container)
{
    if (container->exec_env) {
        wasm_runtime_destroy_exec_env(container->exec_env);
        container->exec_env = NULL;
    }

    if (container->inst) {
        wasm_runtime_deinstantiate(container->inst);
        container->inst = NULL;
    }

    if (container->module) {
        wasm_runtime_unload(container->module);
        container->module = NULL;
    }

    if (container->buffer) {
        free(container->buffer);
        container->buffer = NULL;
    }
}

static void
cleanup_run(scaling_run_t *run)
{
    if (!run) {
        return;
    }

    for (uint32_t i = 0; i < run->container_count; ++i) {
        cleanup_container(&run->containers[i]);
    }

    pthread_cond_destroy(&run->start_cond);
    pthread_mutex_destroy(&run->state_lock);
}

static void
init_cpu_measurement(void)
{
    uint32_t total_runtime = 0;

    uxTaskGetSystemState(g_task_status_array, SCALING_MAX_TASKS, &total_runtime);
    g_prev_total_runtime = total_runtime;
}

static void
calculate_cpu_metrics(TaskStatus_t *task_status,
                      UBaseType_t num_returned,
                      uint32_t delta_total,
                      float *core_usage_0,
                      float *core_usage_1,
                      float *overall_usage)
{
    TaskHandle_t idle_handle_0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle_handle_1 = xTaskGetIdleTaskHandleForCore(1);
    uint32_t idle_ticks_0 = 0;
    uint32_t idle_ticks_1 = 0;
    static uint32_t prev_idle_ticks_0 = 0;
    static uint32_t prev_idle_ticks_1 = 0;
    uint32_t delta_idle_0;
    uint32_t delta_idle_1;

    for (UBaseType_t i = 0; i < num_returned; ++i) {
        TaskStatus_t *t = &task_status[i];

        if (t->xHandle == idle_handle_0) {
            idle_ticks_0 = t->ulRunTimeCounter;
        }
        else if (t->xHandle == idle_handle_1) {
            idle_ticks_1 = t->ulRunTimeCounter;
        }
    }

    delta_idle_0 = (idle_ticks_0 >= prev_idle_ticks_0)
                       ? (idle_ticks_0 - prev_idle_ticks_0)
                       : 0;
    delta_idle_1 = (idle_ticks_1 >= prev_idle_ticks_1)
                       ? (idle_ticks_1 - prev_idle_ticks_1)
                       : 0;
    prev_idle_ticks_0 = idle_ticks_0;
    prev_idle_ticks_1 = idle_ticks_1;

    *core_usage_0 = 0.0f;
    *core_usage_1 = 0.0f;

    if (delta_total > 0U) {
        float idle_percent_0 = ((float)delta_idle_0 / (float)delta_total) * 100.0f;
        float idle_percent_1 = ((float)delta_idle_1 / (float)delta_total) * 100.0f;

        *core_usage_0 = 100.0f - idle_percent_0;
        *core_usage_1 = 100.0f - idle_percent_1;

        if (*core_usage_0 < 0.0f) {
            *core_usage_0 = 0.0f;
        }
        if (*core_usage_0 > 100.0f) {
            *core_usage_0 = 100.0f;
        }
        if (*core_usage_1 < 0.0f) {
            *core_usage_1 = 0.0f;
        }
        if (*core_usage_1 > 100.0f) {
            *core_usage_1 = 100.0f;
        }
    }

    *overall_usage = (*core_usage_0 + *core_usage_1) * 0.5f;
}

static void
sample_cpu_usage(cpu_mean_accumulator_t *acc)
{
    uint32_t total_runtime = 0;
    UBaseType_t num_tasks;
    uint32_t delta_total;
    float core0 = 0.0f;
    float core1 = 0.0f;
    float overall = 0.0f;

    num_tasks = uxTaskGetSystemState(g_task_status_array, SCALING_MAX_TASKS,
                                     &total_runtime);
    delta_total = total_runtime - g_prev_total_runtime;
    g_prev_total_runtime = total_runtime;

    calculate_cpu_metrics(g_task_status_array, num_tasks, delta_total,
                          &core0, &core1, &overall);

    acc->sum_core0 += core0;
    acc->sum_core1 += core1;
    acc->sum_overall += overall;
    acc->sample_count++;
}

static void *
worker_thread_entry(void *arg)
{
    worker_thread_arg_t *thread_arg = (worker_thread_arg_t *)arg;
    scaling_run_t *run = thread_arg->run;
    scaling_container_t *container = &run->containers[thread_arg->container_index];
    wasm_function_inst_t main_func;
    uint32_t argv[2] = {0, 0};
    bool should_run = false;

    container->exec_env = wasm_runtime_create_exec_env(container->inst,
                                                       SCALING_EXEC_ENV_STACK_BYTES);

    pthread_mutex_lock(&run->state_lock);
    container->prepared = true;
    run->prepared_count++;

    if (container->exec_env) {
        run->ready_count++;
    }
    else {
        run->launch_failed = true;
        copy_reason(container->failure_reason, sizeof(container->failure_reason),
                    "wasm_runtime_create_exec_env() returned NULL");
    }

    pthread_cond_broadcast(&run->start_cond);

    while (!run->start_released && !run->abort_start) {
        pthread_cond_wait(&run->start_cond, &run->state_lock);
    }

    should_run = container->exec_env && run->start_released && !run->abort_start;
    pthread_mutex_unlock(&run->state_lock);

    if (should_run) {
        main_func = wasm_runtime_lookup_function(container->inst, "main");

        if (!main_func) {
            copy_reason(container->failure_reason, sizeof(container->failure_reason),
                        "No main() export");
        }
        else if (!wasm_runtime_call_wasm(container->exec_env, main_func, 2, argv)) {
            copy_reason(container->failure_reason, sizeof(container->failure_reason),
                        wasm_runtime_get_exception(container->inst));
        }
        else {
            container->call_ok = true;
        }
    }

    pthread_mutex_lock(&run->state_lock);
    container->call_completed = true;
    run->completed_count++;
    pthread_cond_broadcast(&run->start_cond);
    pthread_mutex_unlock(&run->state_lock);

    return NULL;
}

static int
create_pinned_pthread(pthread_t *thread,
                      void *(*func)(void *),
                      void *arg,
                      int core_id,
                      uint32_t stack_size)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();

    cfg.stack_size = stack_size;
    cfg.prio = 5;
    cfg.pin_to_core = core_id;

    if (esp_pthread_set_cfg(&cfg) != ESP_OK) {
        return -1;
    }

    return pthread_create(thread, NULL, func, arg);
}

static void
join_created_threads(scaling_run_t *run)
{
    for (uint32_t i = 0; i < run->created_threads; ++i) {
        pthread_join(run->containers[i].thread, NULL);
    }
}

static void
abort_launch(scaling_run_t *run)
{
    pthread_mutex_lock(&run->state_lock);
    run->abort_start = true;
    pthread_cond_broadcast(&run->start_cond);
    pthread_mutex_unlock(&run->state_lock);
}

static scaling_status_t
wait_for_thread_preparation(scaling_run_t *run)
{
    int64_t deadline_us = esp_timer_get_time() + (SCALING_PREPARE_TIMEOUT_MS * 1000LL);

    while (true) {
        uint32_t prepared = 0;
        bool launch_failed = false;

        pthread_mutex_lock(&run->state_lock);
        prepared = run->prepared_count;
        launch_failed = run->launch_failed;
        pthread_mutex_unlock(&run->state_lock);

        if (launch_failed) {
            return SCALE_STATUS_EXEC_ENV_FAILED;
        }
        if (prepared == run->created_threads) {
            return SCALE_STATUS_OK;
        }
        if (esp_timer_get_time() >= deadline_us) {
            return SCALE_STATUS_INCOMPLETE;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static scaling_status_t
monitor_execution(scaling_run_t *run, scaling_csv_row_t *row)
{
    cpu_mean_accumulator_t cpu_acc = {0};
    int64_t next_sample_us;
    int64_t deadline_us;
    uint32_t steady_heap_min;

    init_cpu_measurement();

    next_sample_us = run->start_release_us
                     + (SCALING_CPU_SAMPLE_INTERVAL_MS * 1000LL);
    deadline_us = run->start_release_us + (SCALING_RUN_TIMEOUT_MS * 1000LL);
    steady_heap_min = esp_get_free_heap_size();

    while (true) {
        uint32_t completed = 0;
        int64_t now_us = esp_timer_get_time();
        uint32_t free_heap = esp_get_free_heap_size();

        if (free_heap < steady_heap_min) {
            steady_heap_min = free_heap;
        }

        while (now_us >= next_sample_us) {
            sample_cpu_usage(&cpu_acc);
            next_sample_us += (SCALING_CPU_SAMPLE_INTERVAL_MS * 1000LL);
        }

        pthread_mutex_lock(&run->state_lock);
        completed = run->completed_count;
        pthread_mutex_unlock(&run->state_lock);

        if (completed == run->container_count) {
            break;
        }

        if (now_us >= deadline_us) {
            run->timed_out = true;

            for (uint32_t i = 0; i < run->container_count; ++i) {
                if (run->containers[i].inst && !run->containers[i].call_completed) {
                    wasm_runtime_terminate(run->containers[i].inst);
                }
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(SCALING_MONITOR_POLL_MS));
    }

    row->free_heap_steady = steady_heap_min;

    if (cpu_acc.sample_count > 0) {
        row->cpu_core0_mean = (float)(cpu_acc.sum_core0 / cpu_acc.sample_count);
        row->cpu_core1_mean = (float)(cpu_acc.sum_core1 / cpu_acc.sample_count);
        row->cpu_overall_mean = (float)(cpu_acc.sum_overall / cpu_acc.sample_count);
    }

    return run->timed_out ? SCALE_STATUS_TIMEOUT : SCALE_STATUS_OK;
}

static scaling_status_t
evaluate_execution(const scaling_run_t *run, scaling_csv_row_t *row)
{
    const uint32_t expected_checksum = scaling_worker_expected_checksum();
    uint64_t latency_sum = 0;
    uint64_t latency_max = 0;
    uint32_t latency_count = 0;

    for (uint32_t i = 0; i < run->container_count; ++i) {
        const scaling_container_t *container = &run->containers[i];

        if (!container->call_completed) {
            return SCALE_STATUS_INCOMPLETE;
        }
        if (!container->call_ok) {
            return SCALE_STATUS_WASM_EXCEPTION;
        }
        if (!container->started || !container->checksum_reported) {
            return SCALE_STATUS_INCOMPLETE;
        }
        if (container->checksum != expected_checksum) {
            ESP_LOGE(TAG,
                     "Checksum mismatch for container %lu: expected=%lu got=%lu",
                     (unsigned long)i,
                     (unsigned long)expected_checksum,
                     (unsigned long)container->checksum);
            return SCALE_STATUS_RESULT_MISMATCH;
        }

        latency_sum += (uint64_t)container->first_exec_latency_us;
        if ((uint64_t)container->first_exec_latency_us > latency_max) {
            latency_max = (uint64_t)container->first_exec_latency_us;
        }
        latency_count++;
    }

    if (latency_count > 0) {
        row->first_exec_latency_mean_us = latency_sum / latency_count;
        row->first_exec_latency_max_us = latency_max;
    }

    return SCALE_STATUS_OK;
}

static scaling_status_t
run_container_count(const scaling_mode_config_t *mode,
                    uint32_t container_count,
                    uint32_t highest_successful_n,
                    scaling_csv_row_t *row)
{
    scaling_run_t run;
    scaling_status_t status = SCALE_STATUS_OK;
    int64_t startup_begin_us = esp_timer_get_time();
    uint32_t loaded_count = 0;
    uint32_t instantiated_count = 0;

    memset(&run, 0, sizeof(run));
    run.container_count = container_count;
    pthread_mutex_init(&run.state_lock, NULL);
    pthread_cond_init(&run.start_cond, NULL);

    memset(row, 0, sizeof(*row));
    row->mode = mode->mode_name;
    row->container_count = container_count;
    row->free_heap_before = g_free_heap_before_init;
    row->free_heap_after_init = g_free_heap_after_init;
    row->highest_successful_n = highest_successful_n;
    row->free_heap_after_load = esp_get_free_heap_size();
    row->free_heap_after_instantiate = row->free_heap_after_load;
    row->free_heap_steady = row->free_heap_after_load;

    ESP_LOGI(TAG, "Mode=%s, N=%lu",
             mode->mode_name, (unsigned long)container_count);

    for (uint32_t i = 0; i < container_count; ++i) {
        status = load_container_module(mode->module_path, &run.containers[i]);
        row->load_time_total_us += run.containers[i].load_time_us;

        if (status != SCALE_STATUS_OK) {
            row->free_heap_after_load = esp_get_free_heap_size();
            row->free_heap_after_instantiate = row->free_heap_after_load;
            row->free_heap_steady = row->free_heap_after_load;
            row->startup_time_total_us =
                (uint64_t)(esp_timer_get_time() - startup_begin_us);
            row->status = status;

            if (loaded_count > 0) {
                row->load_time_mean_us = row->load_time_total_us / loaded_count;
            }

            cleanup_run(&run);
            return status;
        }

        loaded_count++;
    }

    row->free_heap_after_load = esp_get_free_heap_size();
    row->free_heap_after_instantiate = row->free_heap_after_load;
    row->free_heap_steady = row->free_heap_after_load;
    row->load_time_mean_us = row->load_time_total_us / loaded_count;

    for (uint32_t i = 0; i < container_count; ++i) {
        status = instantiate_container(&run.containers[i]);
        row->instantiate_time_total_us += run.containers[i].instantiate_time_us;

        if (status != SCALE_STATUS_OK) {
            row->free_heap_after_instantiate = esp_get_free_heap_size();
            row->free_heap_steady = row->free_heap_after_instantiate;
            row->startup_time_total_us =
                (uint64_t)(esp_timer_get_time() - startup_begin_us);
            row->status = status;

            if (instantiated_count > 0) {
                row->instantiate_time_mean_us =
                    row->instantiate_time_total_us / instantiated_count;
            }

            cleanup_run(&run);
            return status;
        }

        instantiated_count++;
    }

    row->free_heap_after_instantiate = esp_get_free_heap_size();
    row->free_heap_steady = row->free_heap_after_instantiate;
    row->instantiate_time_mean_us =
        row->instantiate_time_total_us / instantiated_count;

    for (uint32_t i = 0; i < container_count; ++i) {
        int core_id = (int)(i % 2u);

        run.thread_args[i].run = &run;
        run.thread_args[i].container_index = i;

        if (create_pinned_pthread(&run.containers[i].thread,
                                  worker_thread_entry,
                                  &run.thread_args[i],
                                  core_id,
                                  SCALING_PTHREAD_STACK_BYTES) != 0) {
            row->status = SCALE_STATUS_PTHREAD_FAILED;
            row->startup_time_total_us =
                (uint64_t)(esp_timer_get_time() - startup_begin_us);
            row->free_heap_steady = esp_get_free_heap_size();

            abort_launch(&run);
            join_created_threads(&run);
            cleanup_run(&run);
            return SCALE_STATUS_PTHREAD_FAILED;
        }

        run.containers[i].thread_created = true;
        run.created_threads++;
    }

    status = wait_for_thread_preparation(&run);
    if (status != SCALE_STATUS_OK || run.ready_count != container_count) {
        row->status = status;
        row->startup_time_total_us =
            (uint64_t)(esp_timer_get_time() - startup_begin_us);
        row->free_heap_steady = esp_get_free_heap_size();

        abort_launch(&run);
        join_created_threads(&run);
        cleanup_run(&run);
        return status;
    }

    g_active_run = &run;

    pthread_mutex_lock(&run.state_lock);
    run.start_release_us = esp_timer_get_time();
    run.start_released = true;
    pthread_cond_broadcast(&run.start_cond);
    pthread_mutex_unlock(&run.state_lock);

    row->startup_time_total_us =
        (uint64_t)(run.start_release_us - startup_begin_us);

    status = monitor_execution(&run, row);

    join_created_threads(&run);
    g_active_run = NULL;

    if (status == SCALE_STATUS_OK) {
        status = evaluate_execution(&run, row);
    }

    row->status = status;

    cleanup_run(&run);
    return status;
}

static void
append_row(const scaling_csv_row_t *row)
{
    if (g_row_count < (sizeof(g_rows) / sizeof(g_rows[0]))) {
        g_rows[g_row_count++] = *row;
    }
}

static void
run_mode_sweep(const scaling_mode_config_t *mode, scaling_mode_summary_t *summary)
{
    scaling_csv_row_t row;
    scaling_status_t status = SCALE_STATUS_OK;

    summary->mode = mode->mode_name;
    summary->highest_successful_n = 0;
    summary->terminal_status = SCALE_STATUS_OK;

    for (uint32_t n = 1; n <= SCALING_MAX_CONTAINERS; ++n) {
        status = run_container_count(mode, n, summary->highest_successful_n, &row);

        if (status == SCALE_STATUS_OK) {
            summary->highest_successful_n = n;
            row.highest_successful_n = n;
        }
        else {
            row.highest_successful_n = summary->highest_successful_n;
            summary->terminal_status = status;
        }

        append_row(&row);

        if (status != SCALE_STATUS_OK) {
            ESP_LOGW(TAG,
                     "Stopping %s sweep at N=%lu (%s)",
                     mode->mode_name,
                     (unsigned long)n,
                     status_to_string(status));
            return;
        }
    }

    ESP_LOGW(TAG,
             "%s sweep reached SCALING_MAX_CONTAINERS=%d without failure",
             mode->mode_name,
             SCALING_MAX_CONTAINERS);
}

static void
print_scaling_csv(void)
{
    printf("\n<<<CSV_START>>>\n");
    printf("mode,container_count,status,load_time_total_us,load_time_mean_us,instantiate_time_total_us,instantiate_time_mean_us,startup_time_total_us,first_exec_latency_mean_us,first_exec_latency_max_us,free_heap_before,free_heap_after_init,free_heap_after_load,free_heap_after_instantiate,free_heap_steady,cpu_core0_mean,cpu_core1_mean,cpu_overall_mean,highest_successful_n\n");

    for (size_t i = 0; i < g_row_count; ++i) {
        const scaling_csv_row_t *row = &g_rows[i];

        printf("%s,%lu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f,%lu\n",
               row->mode,
               (unsigned long)row->container_count,
               status_to_string(row->status),
               (unsigned long long)row->load_time_total_us,
               (unsigned long long)row->load_time_mean_us,
               (unsigned long long)row->instantiate_time_total_us,
               (unsigned long long)row->instantiate_time_mean_us,
               (unsigned long long)row->startup_time_total_us,
               (unsigned long long)row->first_exec_latency_mean_us,
               (unsigned long long)row->first_exec_latency_max_us,
               (unsigned long)row->free_heap_before,
               (unsigned long)row->free_heap_after_init,
               (unsigned long)row->free_heap_after_load,
               (unsigned long)row->free_heap_after_instantiate,
               (unsigned long)row->free_heap_steady,
               row->cpu_core0_mean,
               row->cpu_core1_mean,
               row->cpu_overall_mean,
               (unsigned long)row->highest_successful_n);
    }

    printf("<<<CSV_END>>>\n\n");
    fflush(stdout);
}

static void
print_scaling_summary(void)
{
    printf("\n<<<SUMMARY_START>>>\n");
    printf("mode,highest_successful_n,terminal_status\n");

    for (size_t i = 0; i < sizeof(g_mode_summaries) / sizeof(g_mode_summaries[0]); ++i) {
        printf("%s,%lu,%s\n",
               g_mode_summaries[i].mode,
               (unsigned long)g_mode_summaries[i].highest_successful_n,
               status_to_string(g_mode_summaries[i].terminal_status));
    }

    printf("<<<SUMMARY_END>>>\n\n");
    fflush(stdout);
}

static void
print_task_runtime_stats(void)
{
    char *stats_buffer = malloc(2048);

    if (!stats_buffer) {
        ESP_LOGE(TAG, "Failed to allocate runtime stats buffer");
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

static bool
wasm_host_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    RuntimeInitArgs init_args;
    uint32_t num_symbols = sizeof(native_symbols) / sizeof(native_symbols[0]);

    g_free_heap_before_init = esp_get_free_heap_size();

    if (esp_vfs_spiffs_register(&conf) != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed");
        return false;
    }

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "WAMR init failed");
        esp_vfs_spiffs_unregister("storage");
        return false;
    }

    wasm_runtime_register_natives("env", native_symbols, num_symbols);
    g_free_heap_after_init = esp_get_free_heap_size();

    ESP_LOGI(TAG,
             "Scaling benchmark ready: free_heap_before=%lu free_heap_after_init=%lu expected_checksum=0x%08lx",
             (unsigned long)g_free_heap_before_init,
             (unsigned long)g_free_heap_after_init,
             (unsigned long)scaling_worker_expected_checksum());

    return true;
}

static void
wasm_host_shutdown(void)
{
    wasm_runtime_destroy();
    esp_vfs_spiffs_unregister("storage");
}

static void *
benchmark_thread_entry(void *arg)
{
    (void)arg;

    if (!wasm_host_init()) {
        return NULL;
    }

    memset(g_rows, 0, sizeof(g_rows));
    memset(g_mode_summaries, 0, sizeof(g_mode_summaries));
    g_row_count = 0;

    for (size_t i = 0; i < sizeof(scaling_modes) / sizeof(scaling_modes[0]); ++i) {
        run_mode_sweep(&scaling_modes[i], &g_mode_summaries[i]);
    }

    print_scaling_csv();
    print_scaling_summary();
    print_task_runtime_stats();

    wasm_host_shutdown();

    ESP_LOGI(TAG, "Scaling benchmark complete. Free heap: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    return NULL;
}

void
app_main(void)
{
    pthread_t bench_thread;
    esp_pthread_cfg_t pthread_cfg = esp_pthread_get_default_config();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "WAMR Multi-Container Scaling Benchmark");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Free heap at start: %lu bytes",
             (unsigned long)esp_get_free_heap_size());

    pthread_cfg.stack_size = SCALING_BENCHMARK_THREAD_STACK_BYTES;
    pthread_cfg.prio = 5;
    esp_pthread_set_cfg(&pthread_cfg);

    if (pthread_create(&bench_thread, NULL, benchmark_thread_entry, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to create benchmark pthread");
        return;
    }

    pthread_join(bench_thread, NULL);
}
