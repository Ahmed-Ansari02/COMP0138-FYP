/* controller/main/main.c */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "wasm_export.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/dac_oneshot.h"
#include "esp_heap_caps.h"      // For heap metrics
#include "esp_system.h"         // For system info
#include "esp_freertos_hooks.h" // CPU Usage
#include "esp_pthread.h"        // Add this include at the top

#define TAG "CONTROLLER"
#define METRICS_TAG "METRICS"
#define SYSTEM_LOGGING_ENABLED 0
#define LOG_METRICS_ENABLED 0
#define RECORD_METRICS 1
#define PRINT_TO_CSV 1
#define NO_VALUES_TO_SAVE 1000
#define MAX_TASKS 20
#define CPU_MEASUREMENT_INTERVAL_MS 100
#define ENABLE_CPU_SAMPLER 0
#define E2E_THRESHOLD 0.5f
#define MAX_SWAPS 10
#define MAX_FAULTS 5
#define WASM_WATCHDOG_TIMEOUT_US (5 * 1000 * 1000) // 5 seconds

// Test mode selector (mirrors native):
//   0 = E2E step response (default)
//   1 = Hot-swap test (bang-bang -> PID)
//   2 = Fault: OOB memory access
//   3 = Fault: Stack overflow
//   4 = Fault: Infinite loop
//   5 = Bang-bang finite run (CSV then exit)
//   6 = PID finite run (CSV then exit)
#define TEST_MODE 5

// Fault modes are only 2, 3, 4
#define IS_FAULT_TEST_MODE ((TEST_MODE >= 2) && (TEST_MODE <= 4))

// WASM execution mode: 0 = interpreter (.wasm), 1 = AOT (.aot)
#define USE_AOT 0
#define CONTROL_ALGORITHM_CORE 0

#if (CONTROL_ALGORITHM_CORE != 0) && (CONTROL_ALGORITHM_CORE != 1)
#error "CONTROL_ALGORITHM_CORE must be 0 or 1"
#endif

#if USE_AOT
#define WASM_EXT ".aot"
#else
#define WASM_EXT ".wasm"
#endif

// connection schematic to bridge
// GPIO 32 -> GPIO 25 (ADC reads DAC)
// GPIO 27 -> GPIO 27 (PWM output to capture)

#define PIN_ADC_CHAN ADC_CHANNEL_4 // GPIO 32
#define ADC_ATTEN ADC_ATTEN_DB_12
#define RESET_SIGNAL_GPIO 13

static float current_temp = 25.0f; // Current temperature from bridge
static SemaphoreHandle_t temp_mutex = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static dac_oneshot_handle_t heater_dac;
static TaskStatus_t task_status_array[MAX_TASKS];
static uint32_t prev_total_runtime = 0;

// ============================================================================
// PERFORMANCE METRICS STRUCTURES
// ============================================================================
// CPU Usage - FreeRTOS Runtime Stats based measurement (dual-core)
typedef struct
{
    float overall_cpu_usage;
    float core_0_usage;
    float core_1_usage;
} cpu_stats_t;

// ============================================================================
// PERFORMANCE METRICS STRUCTURES
// ============================================================================

typedef struct
{
    // Timing metrics (Previously WASM, now Native Algo)
    int64_t total_exec_time_us;
    uint32_t call_count;
    int64_t exec_times[NO_VALUES_TO_SAVE];

    // Control loop metrics
    int64_t last_loop_time_us;
    int64_t total_loop_time_us;
    uint32_t loop_iterations;
    int64_t loop_times[NO_VALUES_TO_SAVE];

    // ADC read metrics
    int64_t total_adc_read_time_us;
    uint32_t adc_read_count;
    int64_t adc_times[NO_VALUES_TO_SAVE];
    uint32_t free_heap_bytes[NO_VALUES_TO_SAVE];
    float temperature_c[NO_VALUES_TO_SAVE];

    // cpu logging
    cpu_stats_t cpu_stats[NO_VALUES_TO_SAVE];
    uint32_t cpu_log_count;

    // Heater set metrics
    int64_t total_heater_set_time_us;
    uint32_t heater_set_count;
    int64_t heater_set_times[NO_VALUES_TO_SAVE];
    // Temperature get metrics
    int64_t total_temperature_get_time_us;
    uint32_t temperature_get_count;
    int64_t temperature_get_times[NO_VALUES_TO_SAVE];

    // e2e delay metrics
    int64_t total_e2e_delay_us;
    uint32_t e2e_delay_count;
    int64_t command_send_time_us;
    float temp_at_command;
    bool waiting_for_rise;
    int64_t e2e_times[NO_VALUES_TO_SAVE];

    // Mutex wait-time metrics (reader_task on Core 1)
    int64_t reader_temp_mutex_wait_us[NO_VALUES_TO_SAVE];
    int64_t reader_metrics_mutex_wait_us[NO_VALUES_TO_SAVE];

} perf_metrics_t;

// Hot-swap metrics
typedef struct
{
    int64_t swap_latency_us[MAX_SWAPS];   // Time between containers (load + instantiate)
    int64_t load_time_us[MAX_SWAPS];      // SPIFFS read time
    int64_t instantiate_time_us[MAX_SWAPS]; // WAMR load + instantiate time
    float temp_at_swap[MAX_SWAPS];         // Temperature when swap initiated
    float temp_after_swap[MAX_SWAPS];      // Temperature after new container starts
    int64_t swap_timestamp_us[MAX_SWAPS];  // Absolute time of each swap
    uint32_t swap_count;
} swap_metrics_t;

static swap_metrics_t swap_metrics = {0};

// WASM run result
typedef enum {
    WASM_RESULT_OK = 0,
    WASM_RESULT_EXCEPTION = 1,   // WAMR caught fault (OOB, stack overflow)
    WASM_RESULT_TERMINATED = 2,  // Watchdog killed it (infinite loop)
    WASM_RESULT_LOAD_FAILED = 3,
} wasm_result_t;

#if IS_FAULT_TEST_MODE
// Watchdog state
static volatile int64_t last_wasm_activity_us = 0;
static volatile wasm_module_inst_t active_module_inst = NULL;

// Fault metrics
typedef struct {
    int64_t fault_timestamp_us[MAX_FAULTS];
    int64_t detection_latency_us[MAX_FAULTS];
    int64_t recovery_latency_us[MAX_FAULTS];
    int fault_type[MAX_FAULTS];                  // EXCEPTION=1 or TERMINATED=2
    char fault_description[MAX_FAULTS][64];
    float temp_at_fault[MAX_FAULTS];
    uint32_t heap_at_fault[MAX_FAULTS];
    bool adc_reader_alive[MAX_FAULTS];
    float temp_after_recovery[MAX_FAULTS];
    uint32_t heap_after_recovery[MAX_FAULTS];
    uint32_t fault_count;
} fault_metrics_t;

static fault_metrics_t fault_metrics = {0};

// Integrity check result
typedef struct {
    bool temp_valid;       // temp in [0, 100]
    bool adc_alive;        // adc_read_count incremented
    bool mutex_ok;         // metrics_mutex takeable
    bool heap_ok;          // free heap > 20KB
    uint32_t free_heap;
    float current_temp;
} integrity_check_t;
#endif

static void _calculate_cpu_metrics(TaskStatus_t *task_status, UBaseType_t num_returned, uint32_t delta_total,
                                   float *core_usage_0, float *core_usage_1, float *overall_usage)
{
    TaskHandle_t idle_handle_0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle_handle_1 = xTaskGetIdleTaskHandleForCore(1);

    uint32_t idle_ticks_0 = 0;
    uint32_t idle_ticks_1 = 0;
    for (UBaseType_t i = 0; i < num_returned; i++)
    {
        TaskStatus_t *t = &task_status[i];
        if (t->xHandle == idle_handle_0)
        {
            idle_ticks_0 = t->ulRunTimeCounter;
        }
        else if (t->xHandle == idle_handle_1)
        {
            idle_ticks_1 = t->ulRunTimeCounter;
        }
    }

    // Maintain state between iterations
    static uint32_t prev_idle_ticks_0 = 0;
    static uint32_t prev_idle_ticks_1 = 0;
    uint32_t delta_idle_0 = (idle_ticks_0 >= prev_idle_ticks_0) ? (idle_ticks_0 - prev_idle_ticks_0) : 0;
    uint32_t delta_idle_1 = (idle_ticks_1 >= prev_idle_ticks_1) ? (idle_ticks_1 - prev_idle_ticks_1) : 0;
    prev_idle_ticks_0 = idle_ticks_0;
    prev_idle_ticks_1 = idle_ticks_1;

    *core_usage_0 = 0.0f;
    *core_usage_1 = 0.0f;
    if (delta_total > 0U)
    {
        float idle_percent_0 = ((float)delta_idle_0 / (float)delta_total) * 100.0f;
        float idle_percent_1 = ((float)delta_idle_1 / (float)delta_total) * 100.0f;
        *core_usage_0 = 100.0f - idle_percent_0;
        *core_usage_1 = 100.0f - idle_percent_1;

        // Clamp to valid range
        if (*core_usage_0 < 0.0f)
        {
            *core_usage_0 = 0.0f;
        }
        if (*core_usage_0 > 100.0f)
        {
            *core_usage_0 = 100.0f;
        }
        if (*core_usage_1 < 0.0f)
        {
            *core_usage_1 = 0.0f;
        }
        if (*core_usage_1 > 100.0f)
        {
            *core_usage_1 = 100.0f;
        }
    }
    *overall_usage = (*core_usage_0 + *core_usage_1) * 0.5f;
}

void init_cpu_measurement(void)
{
    // Initialize by doing a first read to establish baseline
    uint32_t total_runtime;
    uxTaskGetSystemState(task_status_array, MAX_TASKS, &total_runtime);
    prev_total_runtime = total_runtime;
    ESP_LOGI(TAG, "CPU measurement initialized (dual-core runtime stats)");
}

static perf_metrics_t metrics = {0};

static SemaphoreHandle_t metrics_mutex = NULL;

void init_metrics(void)
{
    metrics_mutex = xSemaphoreCreateMutex();
    memset(&metrics, 0, sizeof(perf_metrics_t));
}

void init_adc()
{
    // ------------- 1. Setup ADC (Hardware) -------------
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = 0,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Configure the specific channel (Pin 33)
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, PIN_ADC_CHAN, &config));
}

void init_heater_dac()
{
    dac_oneshot_config_t cfg = {.chan_id = DAC_CHAN_0}; // GPIO26
    dac_oneshot_new_channel(&cfg, &heater_dac);
}

// --- SIMULATOR RESET ---
void reset_simulator(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << RESET_SIGNAL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "Resetting simulator (GPIO%d high)...", RESET_SIGNAL_GPIO);
    gpio_set_level(RESET_SIGNAL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(RESET_SIGNAL_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Simulator reset complete");
}

void log_memory_stats(void)
{
    // Heap statistics
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    ESP_LOGI(METRICS_TAG, "=== MEMORY STATS ===");
    ESP_LOGI(METRICS_TAG, "Free heap: %u bytes", free_heap);
    ESP_LOGI(METRICS_TAG, "Min free heap (watermark): %u bytes", min_free_heap);
    ESP_LOGI(METRICS_TAG, "Free internal RAM: %u bytes", free_internal);
    ESP_LOGI(METRICS_TAG, "Free DMA-capable: %u bytes", free_dma);
    ESP_LOGI(METRICS_TAG, "Largest free block: %u bytes", largest_block);
}

void log_task_stats(void)
{
    // #if configUSE_TRACE_FACILITY && configGENERATE_RUN_TIME_STATS
    //     char *task_list_buffer = malloc(1024);
    //     if (task_list_buffer)
    //     {
    //         ESP_LOGI(METRICS_TAG, "=== TASK STATS ===");
    //         vTaskGetRunTimeStats(task_list_buffer);
    //         ESP_LOGI(METRICS_TAG, task_list_buffer);
    //         free(task_list_buffer);
    //     }
    // #else
    ESP_LOGI(METRICS_TAG, "=== TASK STATS ===");
    ESP_LOGI(METRICS_TAG, "Running tasks: %u", uxTaskGetNumberOfTasks());
    // #endif
}

void log_timing_metrics(void)
{
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        ESP_LOGI(METRICS_TAG, "=== TIMING METRICS ===");

        if (metrics.call_count > 0)
        {
            int64_t avg_exec = metrics.total_exec_time_us / metrics.call_count;
            ESP_LOGI(METRICS_TAG, "WASM Calls: %lu | Avg: %lld us",
                     metrics.call_count, avg_exec);
        }

        if (metrics.loop_iterations > 0)
        {
            int64_t avg_loop = metrics.total_loop_time_us / metrics.loop_iterations;
            ESP_LOGI(METRICS_TAG, "Control Loops: %lu | Avg: %lld us",
                     metrics.loop_iterations, avg_loop);
        }

        if (metrics.adc_read_count > 0)
        {
            int64_t avg_adc = metrics.total_adc_read_time_us / metrics.adc_read_count;
            ESP_LOGI(METRICS_TAG, "ADC Reads: %lu | Avg: %lld us",
                     metrics.adc_read_count, avg_adc);
        }

        if (metrics.heater_set_count > 0)
        {
            int64_t avg_heater = metrics.total_heater_set_time_us / metrics.heater_set_count;
            ESP_LOGI(METRICS_TAG, "Heater Sets: %lu | Avg: %lld us",
                     metrics.heater_set_count, avg_heater);
        }

        xSemaphoreGive(metrics_mutex);
    }
}

void log_e2e_metrics(void)
{
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        ESP_LOGI(METRICS_TAG, "=== END-TO-END DELAY METRICS ===");

        if (metrics.e2e_delay_count > 0)
        {
            int64_t avg_e2e = metrics.total_e2e_delay_us / metrics.e2e_delay_count;
            ESP_LOGI(METRICS_TAG, "E2E Measurements: %lu | Avg: %lld us",
                     metrics.e2e_delay_count, avg_e2e);
        }

        xSemaphoreGive(metrics_mutex);
    }
}

void print_task_runtime_stats(void)
{
    char *stats_buffer = malloc(2048);
    if (stats_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate stats buffer");
        return;
    }

    ESP_LOGI(TAG, "Generating runtime stats...");
    vTaskGetRunTimeStats(stats_buffer);

    printf("\n<<<RUNTIME_STATS_START>>>\n");
    printf("Task            Abs Time      %% Time\n");
    printf("%s", stats_buffer);
    printf("<<<RUNTIME_STATS_END>>>\n\n");
    fflush(stdout);

    free(stats_buffer);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void print_metrics_csv(void)
{
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to acquire mutex for CSV output");
        return;
    }

    // Determine how many values we actually have for each metric
    uint32_t algo_count = (metrics.call_count < NO_VALUES_TO_SAVE) ? metrics.call_count : NO_VALUES_TO_SAVE;
    uint32_t loop_count = (metrics.loop_iterations < NO_VALUES_TO_SAVE) ? metrics.loop_iterations : NO_VALUES_TO_SAVE;
    uint32_t adc_count = (metrics.adc_read_count < NO_VALUES_TO_SAVE) ? metrics.adc_read_count : NO_VALUES_TO_SAVE;
    uint32_t heater_count = (metrics.heater_set_count < NO_VALUES_TO_SAVE) ? metrics.heater_set_count : NO_VALUES_TO_SAVE;
    uint32_t e2e_count = (metrics.e2e_delay_count < NO_VALUES_TO_SAVE) ? metrics.e2e_delay_count : NO_VALUES_TO_SAVE;
    uint32_t temperature_get_count = (metrics.temperature_get_count < NO_VALUES_TO_SAVE) ? metrics.temperature_get_count : NO_VALUES_TO_SAVE;
    uint32_t cpu_count = (metrics.cpu_log_count < NO_VALUES_TO_SAVE) ? metrics.cpu_log_count : NO_VALUES_TO_SAVE;

    // Find the maximum count to determine CSV rows
    uint32_t max_count = algo_count;
    if (loop_count > max_count)
        max_count = loop_count;
    if (adc_count > max_count)
        max_count = adc_count;
    if (heater_count > max_count)
        max_count = heater_count;
    if (e2e_count > max_count)
        max_count = e2e_count;
    if (temperature_get_count > max_count)
        max_count = temperature_get_count;
    if (cpu_count > max_count)
        max_count = cpu_count;

    ESP_LOGI(METRICS_TAG, "=== PRINTING CSV TO SERIAL ===");
    ESP_LOGI(METRICS_TAG, "Total samples: algo=%lu, loop=%lu, adc=%lu, heater=%lu, e2e=%lu, temperature_get=%lu",
             algo_count, loop_count, adc_count, heater_count, e2e_count, temperature_get_count);

    // Small delay to ensure log messages are flushed
    vTaskDelay(pdMS_TO_TICKS(100));

    // Print CSV to serial - use printf for clean output without log prefixes
    printf("\n<<<CSV_START>>>\n");
    printf("index,exec_time_us,loop_time_us,adc_time_us,heater_time_us,temp_get_time_us,e2e_time_us,temp_c,free_heap,cpu_core0,cpu_core1,cpu_overall,temp_mutex_wait,metrics_mutex_wait\n");

    // Print CSV data rows
    for (uint32_t i = 0; i < max_count; i++)
    {
        printf("%lu,", i);

        // Algo times
        if (i < algo_count)
            printf("%lld,", metrics.exec_times[i]);
        else
            printf(",");

        // Loop times
        if (i < loop_count)
            printf("%lld,", metrics.loop_times[i]);
        else
            printf(",");

        // ADC times
        if (i < adc_count)
            printf("%lld,", metrics.adc_times[i]);
        else
            printf(",");

        // Heater set times
        if (i < heater_count)
            printf("%lld,", metrics.heater_set_times[i]);
        else
            printf(",");

        // Temperature get times
        if (i < temperature_get_count)
            printf("%lld,", metrics.temperature_get_times[i]);
        else
            printf(",");

        // E2E times
        if (i < e2e_count)
            printf("%lld,", metrics.e2e_times[i]);
        else
            printf(",");

        // Temperature sample (C)
        // If ADC has fewer samples than CSV rows, carry forward the last measured value.
        if (adc_count > 0)
        {
            uint32_t temp_idx = (i < adc_count) ? i : (adc_count - 1);
            printf("%.2f,", metrics.temperature_c[temp_idx]);
        }
        else
        {
            printf(",");
        }

        // Free Heap
        if (i < adc_count)
        {
            printf("%lu,", metrics.free_heap_bytes[i]);
        }
        else
        {
            printf(",");
        }

        // CPU Usage (core 0, core 1, overall)
        if (i < cpu_count)
        {
            printf("%.2f,%.2f,%.2f,",
                   metrics.cpu_stats[i].core_0_usage,
                   metrics.cpu_stats[i].core_1_usage,
                   metrics.cpu_stats[i].overall_cpu_usage);
        }
        else
        {
            printf(",,,");
        }

        // Mutex wait times (reader_task, Core 1)
        if (i < adc_count)
        {
            printf("%lld,%lld\n",
                   metrics.reader_temp_mutex_wait_us[i],
                   metrics.reader_metrics_mutex_wait_us[i]);
        }
        else
        {
            printf(",\n");
        }
    }

    printf("<<<CSV_END>>>\n\n");
    fflush(stdout);

    // Print swap metrics if any swaps occurred
    if (swap_metrics.swap_count > 0)
    {
        printf("\n<<<HOTSWAP_START>>>\n");
        printf("swap_index,swap_latency_us,load_time_us,instantiate_time_us,temp_at_swap,temp_after_swap,swap_timestamp_us\n");
        for (uint32_t i = 0; i < swap_metrics.swap_count; i++)
        {
            printf("%lu,%lld,%lld,%lld,%.2f,%.2f,%lld\n",
                   i,
                   swap_metrics.swap_latency_us[i],
                   swap_metrics.load_time_us[i],
                   swap_metrics.instantiate_time_us[i],
                   swap_metrics.temp_at_swap[i],
                   swap_metrics.temp_after_swap[i],
                   swap_metrics.swap_timestamp_us[i]);
        }
        printf("<<<HOTSWAP_END>>>\n\n");
        fflush(stdout);
    }

    xSemaphoreGive(metrics_mutex);

    ESP_LOGI(METRICS_TAG, "CSV output complete (%lu rows)", max_count);
}

void log_all_metrics(void)
{
    ESP_LOGI(METRICS_TAG, "================================================");
    ESP_LOGI(METRICS_TAG, "       PERFORMANCE METRICS REPORT");
    ESP_LOGI(METRICS_TAG, "================================================");
    log_memory_stats();
    log_task_stats();
    log_timing_metrics();

    log_e2e_metrics();

    ESP_LOGI(METRICS_TAG, "================================================");
}

// ============================================================================
// NATIVE FUNCTIONS (Exposed to WASM) - WITH METRICS
// ============================================================================
// Get current temperature reading from the bridge - METRICS MOVED TO WASM SIDE
float host_get_temperature(wasm_exec_env_t exec_env)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    float temp = 25.0f;
    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        temp = current_temp;
        xSemaphoreGive(temp_mutex);
    }
    return temp;
}

// Set heater command - METRICS MOVED TO WASM SIDE (except E2E Trigger)
void host_set_heater(wasm_exec_env_t exec_env, float value)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    // Clamp value to 0-1 range
    if (value > 1.0f)
        value = 1.0f;
    if (value < 0.0f)
        value = 0.0f;

    // Read current_temp under temp_mutex to avoid data race
    float safe_temp = 25.0f;
    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        safe_temp = current_temp;
        xSemaphoreGive(temp_mutex);
    }

    // Capture timestamp BEFORE DAC write to include DAC latency in E2E measurement
    int64_t now = esp_timer_get_time();

    uint8_t dac_value = (uint8_t)(value * 255);
    dac_oneshot_output_voltage(heater_dac, dac_value);

    if (SYSTEM_LOGGING_ENABLED)
    {
        ESP_LOGI(TAG, "Heater: %.2f -> DAC: %d", value, dac_value);
    }

    // E2E Tracking trigger
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        if (value > 0.5f && !metrics.waiting_for_rise)
        {
            metrics.command_send_time_us = now;
            metrics.temp_at_command = safe_temp;
            metrics.waiting_for_rise = true;
        }
        xSemaphoreGive(metrics_mutex);
    }
}

// Delay function for WASM - WITH LOOP METRICS
void host_delay(wasm_exec_env_t exec_env, int ms)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    // Record loop iteration timing
    int64_t now = esp_timer_get_time();
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        if (metrics.last_loop_time_us > 0)
        {
            int64_t loop_duration = now - metrics.last_loop_time_us;
            metrics.total_loop_time_us += loop_duration;
            metrics.loop_times[metrics.loop_iterations % NO_VALUES_TO_SAVE] = loop_duration;
            metrics.loop_iterations++;
        }
        metrics.last_loop_time_us = now;
        xSemaphoreGive(metrics_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Log function for WASM (prints to ESP32 console)
void host_log(wasm_exec_env_t exec_env, const char *message)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    if (message)
    {
        ESP_LOGI(TAG, "WASM: %s", message);
    }
}

// Get system time in microseconds for Wasm benchmarking
int64_t host_get_time_us(wasm_exec_env_t exec_env)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    return esp_timer_get_time();
}

// Record Wasm-side timing for Heater Set
void host_record_heater_time(wasm_exec_env_t exec_env, int64_t duration_us)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.heater_set_times[metrics.heater_set_count % NO_VALUES_TO_SAVE] = duration_us;
        metrics.heater_set_count++;
        metrics.total_heater_set_time_us += duration_us;
        xSemaphoreGive(metrics_mutex);
    }
}

// Record Wasm-side timing for Temperature Get
void host_record_temp_get_time(wasm_exec_env_t exec_env, int64_t duration_us)
{
#if IS_FAULT_TEST_MODE
    last_wasm_activity_us = esp_timer_get_time();
#endif
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.temperature_get_times[metrics.temperature_get_count % NO_VALUES_TO_SAVE] = duration_us;
        metrics.temperature_get_count++;
        metrics.total_temperature_get_time_us += duration_us;
        xSemaphoreGive(metrics_mutex);
    }
}

// Native symbol registration
static NativeSymbol native_symbols[] = {
    {"host_get_temperature", host_get_temperature, "()f", NULL},
    {"host_set_heater", host_set_heater, "(f)", NULL},
    {"host_delay", host_delay, "(i)", NULL},
    {"host_log", host_log, "($)", NULL},
    {"host_get_time_us", host_get_time_us, "()I", NULL},
    {"host_record_heater_time", host_record_heater_time, "(I)", NULL},
    {"host_record_temp_get_time", host_record_temp_get_time, "(I)", NULL},
};

wasm_result_t run_wasm(uint8_t *buffer, uint32_t size, int64_t *swap_end)
{
    char error_buf[128];
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_result_t result = WASM_RESULT_OK;

    // Load module
    module = wasm_runtime_load(buffer, size, error_buf, sizeof(error_buf));
    if (!module)
    {
        ESP_LOGE(TAG, "WASM load failed: %s", error_buf);
        return WASM_RESULT_LOAD_FAILED;
    }

    module_inst = wasm_runtime_instantiate(module, 16 * 1024, 16 * 1024, error_buf, sizeof(error_buf));
    if (!module_inst)
    {
        ESP_LOGE(TAG, "WASM instantiation failed: %s", error_buf);
        wasm_runtime_unload(module);
        return WASM_RESULT_LOAD_FAILED;
    }

    exec_env = wasm_runtime_create_exec_env(module_inst, 8 * 1024);
    if (!exec_env)
    {
        ESP_LOGE(TAG, "Exec env creation failed");
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        return WASM_RESULT_LOAD_FAILED;
    }

#if IS_FAULT_TEST_MODE
    active_module_inst = module_inst;
    last_wasm_activity_us = esp_timer_get_time();
#endif

    ESP_LOGI(TAG, "Starting WASM Control Module...");

    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, "main");
    if (swap_end != NULL)
    {
        *swap_end = esp_timer_get_time();
    }
    if (func)
    {
        uint32_t args[2] = {0, 0};
        if (!wasm_runtime_call_wasm(exec_env, func, 2, args))
        {
            const char *exception = wasm_runtime_get_exception(module_inst);
            if (exception && strstr(exception, "terminated"))
            {
                ESP_LOGW(TAG, "WASM execution terminated (watchdog)");
                result = WASM_RESULT_TERMINATED;
            }
            else
            {
                ESP_LOGE(TAG, "WASM exception: %s", exception ? exception : "unknown");
                result = WASM_RESULT_EXCEPTION;
            }
        }
        else
        {
            ESP_LOGI(TAG, "WASM execution completed successfully");
        }
    }
    else
    {
        ESP_LOGE(TAG, "No main function found in WASM module");
        result = WASM_RESULT_LOAD_FAILED;
    }

    wasm_runtime_dump_mem_consumption(exec_env);

#if IS_FAULT_TEST_MODE
    active_module_inst = NULL;
    last_wasm_activity_us = 0;
#endif

    // Cleanup
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);

    return result;
}

uint8_t *load_wasm_from_spiffs(const char *filename, uint32_t *size) // time how long it takes to load wasm container as well
{
    ESP_LOGI(TAG, "Opening file: %s", filename);
    FILE *f = fopen(filename, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buffer = malloc(fsize);
    if (!buffer)
    {
        ESP_LOGE(TAG, "Malloc failed");
        fclose(f);
        return NULL;
    }

    fread(buffer, 1, fsize, f);
    fclose(f);
    *size = (uint32_t)fsize;
    return buffer;
}

#if IS_FAULT_TEST_MODE
// ============================================================================
// WATCHDOG TASK (Phase 2 — Fault Tolerance)
// ============================================================================
void wasm_watchdog_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000)); // check every 1s
        int64_t now = esp_timer_get_time();
        int64_t last = last_wasm_activity_us;
        if (last > 0 && active_module_inst != NULL)
        {
            if ((now - last) > WASM_WATCHDOG_TIMEOUT_US)
            {
                ESP_LOGE(TAG, "WATCHDOG: No WASM activity for %lld us — terminating module",
                         now - last);
                wasm_runtime_terminate(active_module_inst);
                last_wasm_activity_us = 0;
            }
        }
    }
}

// ============================================================================
// HOST INTEGRITY VERIFICATION
// ============================================================================
integrity_check_t verify_host_integrity(void)
{
    integrity_check_t check = {0};

    // 1. Temperature in valid range?
    float temp = 25.0f;
    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        temp = current_temp;
        xSemaphoreGive(temp_mutex);
    }
    check.current_temp = temp;
    check.temp_valid = (temp >= 0.0f && temp <= 100.0f);

    // 2. ADC reader still alive? (check if adc_read_count increments over 2s)
    uint32_t count_before = metrics.adc_read_count;
    vTaskDelay(pdMS_TO_TICKS(2000));
    uint32_t count_after = metrics.adc_read_count;
    check.adc_alive = (count_after > count_before);

    // 3. Metrics mutex accessible?
    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
        check.mutex_ok = true;
        xSemaphoreGive(metrics_mutex);
    }
    else
    {
        check.mutex_ok = false;
    }

    // 4. Free heap > 20KB?
    check.free_heap = esp_get_free_heap_size();
    check.heap_ok = (check.free_heap > 20 * 1024);

    ESP_LOGI(TAG, "INTEGRITY: temp=%.1f(%s) adc(%s) mutex(%s) heap=%lu(%s)",
             check.current_temp,
             check.temp_valid ? "OK" : "FAIL",
             check.adc_alive ? "OK" : "FAIL",
             check.mutex_ok ? "OK" : "FAIL",
             check.free_heap,
             check.heap_ok ? "OK" : "FAIL");

    return check;
}

// Print fault metrics CSV
void print_fault_metrics_csv(void)
{
    if (fault_metrics.fault_count == 0)
        return;

    printf("\n<<<FAULT_START>>>\n");
    printf("fault_index,fault_type,detection_latency_us,recovery_latency_us,temp_at_fault,heap_at_fault,adc_alive,temp_after_recovery,heap_after_recovery,fault_description\n");
    for (uint32_t i = 0; i < fault_metrics.fault_count; i++)
    {
        printf("%lu,%d,%lld,%lld,%.2f,%lu,%d,%.2f,%lu,%s\n",
               i,
               fault_metrics.fault_type[i],
               fault_metrics.detection_latency_us[i],
               fault_metrics.recovery_latency_us[i],
               fault_metrics.temp_at_fault[i],
               fault_metrics.heap_at_fault[i],
               fault_metrics.adc_reader_alive[i],
               fault_metrics.temp_after_recovery[i],
               fault_metrics.heap_after_recovery[i],
               fault_metrics.fault_description[i]);
    }
    printf("<<<FAULT_END>>>\n\n");
    fflush(stdout);
}
#endif // IS_FAULT_TEST_MODE

void *wasm_thread_entry(void *arg)
{
    const char **container_list = (const char **)arg;
    int num_containers = 0;
    while (container_list[num_containers] != NULL)
        num_containers++;

    // Setup SPIFFS
    int64_t total_start_time = esp_timer_get_time();
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true};
    if (esp_vfs_spiffs_register(&conf) != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS Mount Failed");
        return NULL;
    }

    // Initialize WAMR using system allocator (more memory available than static pool)
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&init_args))
    {
        ESP_LOGE(TAG, "WAMR Init Failed");
        return NULL;
    }

    // Register native functions (once for all containers)
    wasm_runtime_register_natives("env", native_symbols, sizeof(native_symbols) / sizeof(NativeSymbol));

#if IS_FAULT_TEST_MODE
    // ---- FAULT TEST MODE: run fault container, detect failure, recover ----
    {
        const char *fault_container = container_list[0];
        const char *backup_container = "/spiffs/bang_bang_finite" WASM_EXT;

        ESP_LOGW(TAG, "================================================");
        ESP_LOGW(TAG, "FAULT TEST: Loading %s", fault_container);
        ESP_LOGW(TAG, "================================================");

        // Load and run fault container
        uint32_t file_size = 0;
        uint8_t *wasm_file = load_wasm_from_spiffs(fault_container, &file_size);
        if (!wasm_file)
        {
            ESP_LOGE(TAG, "Failed to load fault container");
            goto cleanup;
        }

        int64_t pre_fault_time = esp_timer_get_time();
        wasm_result_t result = run_wasm(wasm_file, file_size, NULL);
        int64_t fault_detected_time = esp_timer_get_time();
        free(wasm_file);

        if (result != WASM_RESULT_OK)
        {
            ESP_LOGW(TAG, "================================================");
            ESP_LOGW(TAG, "FAULT DETECTED: type=%d", result);
            ESP_LOGW(TAG, "================================================");

            // Record fault metrics
            uint32_t fi = fault_metrics.fault_count;
            if (fi < MAX_FAULTS)
            {
                fault_metrics.fault_timestamp_us[fi] = fault_detected_time;
                fault_metrics.detection_latency_us[fi] = fault_detected_time - pre_fault_time;
                fault_metrics.fault_type[fi] = result;

                // Get temp and heap at fault time
                float temp_at_fault = 25.0f;
                if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    temp_at_fault = current_temp;
                    xSemaphoreGive(temp_mutex);
                }
                fault_metrics.temp_at_fault[fi] = temp_at_fault;
                fault_metrics.heap_at_fault[fi] = esp_get_free_heap_size();

                // Describe the fault
                if (result == WASM_RESULT_EXCEPTION)
                    snprintf(fault_metrics.fault_description[fi], 64, "wasm_exception");
                else if (result == WASM_RESULT_TERMINATED)
                    snprintf(fault_metrics.fault_description[fi], 64, "watchdog_terminated");
                else
                    snprintf(fault_metrics.fault_description[fi], 64, "load_failed");

                // Verify host integrity
                ESP_LOGI(TAG, "Verifying host integrity...");
                integrity_check_t integrity = verify_host_integrity();
                fault_metrics.adc_reader_alive[fi] = integrity.adc_alive;

                // Recovery: load backup container
                ESP_LOGI(TAG, "================================================");
                ESP_LOGI(TAG, "RECOVERY: Loading backup container %s", backup_container);
                ESP_LOGI(TAG, "================================================");

                int64_t recovery_start = esp_timer_get_time();
                uint32_t backup_size = 0;
                uint8_t *backup_file = load_wasm_from_spiffs(backup_container, &backup_size);
                if (backup_file)
                {
                    wasm_result_t backup_result = run_wasm(backup_file, backup_size, NULL);
                    free(backup_file);
                    int64_t recovery_end = esp_timer_get_time();

                    fault_metrics.recovery_latency_us[fi] = recovery_end - recovery_start;

                    // Post-recovery state
                    float temp_after = 25.0f;
                    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
                    {
                        temp_after = current_temp;
                        xSemaphoreGive(temp_mutex);
                    }
                    fault_metrics.temp_after_recovery[fi] = temp_after;
                    fault_metrics.heap_after_recovery[fi] = esp_get_free_heap_size();

                    ESP_LOGI(TAG, "RECOVERY: Backup container finished with result=%d", backup_result);
                }
                else
                {
                    ESP_LOGE(TAG, "RECOVERY: Failed to load backup container!");
                    fault_metrics.recovery_latency_us[fi] = -1;
                }

                fault_metrics.fault_count++;
            }
        }
        else
        {
            ESP_LOGI(TAG, "Fault container completed without fault (unexpected)");
        }
    }
cleanup:
    ;

#else
    // ---- NORMAL MODE: run containers sequentially (hot-swap / e2e / etc) ----
    for (int c = 0; c < num_containers; c++)
    {
        // Record pre-swap state (for swaps after the first container)
        if (c > 0)
        {
            int64_t swap_start = esp_timer_get_time();
            float temp_before = 25.0f;
            if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                temp_before = current_temp;
                xSemaphoreGive(temp_mutex);
            }

            ESP_LOGI(TAG, "================================================");
            ESP_LOGI(TAG, "SWAP %d: Loading container %s", c, container_list[c]);
            ESP_LOGI(TAG, "Temp at swap: %.2f C", temp_before);
            ESP_LOGI(TAG, "================================================");

            // Load from SPIFFS (timed)
            int64_t load_start = esp_timer_get_time();
            uint32_t file_size = 0;
            uint8_t *wasm_file = load_wasm_from_spiffs(container_list[c], &file_size);
            int64_t load_end = esp_timer_get_time();

            if (!wasm_file)
            {
                ESP_LOGE(TAG, "Failed to load container %s", container_list[c]);
                continue;
            }

            // Run container (instantiate + execute + cleanup, all timed)
            int64_t swap_end;
            int64_t inst_start = esp_timer_get_time();
            run_wasm(wasm_file, file_size, &swap_end);
            free(wasm_file);
            int64_t inst_end = esp_timer_get_time();

            // Record swap metrics
            uint32_t idx = swap_metrics.swap_count;
            if (idx < MAX_SWAPS)
            {
                swap_metrics.swap_latency_us[idx] = swap_end - swap_start;
                swap_metrics.load_time_us[idx] = load_end - load_start;
                swap_metrics.instantiate_time_us[idx] = inst_end - inst_start;
                swap_metrics.temp_at_swap[idx] = temp_before;
                swap_metrics.swap_timestamp_us[idx] = swap_start;

                float temp_after = 25.0f;
                if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
                {
                    temp_after = current_temp;
                    xSemaphoreGive(temp_mutex);
                }
                swap_metrics.temp_after_swap[idx] = temp_after;
                swap_metrics.swap_count++;
            }
        }
        else
        {
            // First container — just load and run normally
            ESP_LOGI(TAG, "================================================");
            ESP_LOGI(TAG, "Loading initial container: %s", container_list[c]);
            ESP_LOGI(TAG, "================================================");

            uint32_t file_size = 0;
            uint8_t *wasm_file = load_wasm_from_spiffs(container_list[c], &file_size);
            if (!wasm_file)
            {
                ESP_LOGE(TAG, "Failed to load container %s", container_list[c]);
                return NULL;
            }
            run_wasm(wasm_file, file_size, NULL);
            free(wasm_file);
        }
    }
#endif // IS_FAULT_TEST_MODE

    int64_t total_end_time = esp_timer_get_time();
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.total_exec_time_us += (total_end_time - total_start_time);
        metrics.exec_times[metrics.call_count % NO_VALUES_TO_SAVE] = total_end_time - total_start_time;
        metrics.call_count++;
        xSemaphoreGive(metrics_mutex);
    }

    if (PRINT_TO_CSV)
    {
        print_metrics_csv();
    }

#if IS_FAULT_TEST_MODE
    print_fault_metrics_csv();
#endif

    print_task_runtime_stats();

    return NULL;
}

void calibrate_adc()
{
    // ------------- 2. Setup Calibration (Software) -------------
    // This loads the factory reference voltage from the chip's eFuse
    ESP_LOGI("CTRL", "Setting up calibration scheme...");

    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    // Check if calibration is successful
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Calibration Success");
    }
    else if (ret == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }
}

void reader_task(void *arg)
{   
    const int CONTROL_PERIOD_MS = 100;
    init_adc();
    calibrate_adc();
    while (1)
    {
        int64_t start = esp_timer_get_time();

        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, PIN_ADC_CHAN, &adc_raw));

        int voltage_mv = 0;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv));

        float temperature = ((float)voltage_mv / 3300.0f) * 100.0f;
        int64_t mw_start = esp_timer_get_time();
        if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            int64_t temp_mutex_wait = esp_timer_get_time() - mw_start;
            current_temp = temperature;
            xSemaphoreGive(temp_mutex);

            if (RECORD_METRICS)
            {
                metrics.reader_temp_mutex_wait_us[metrics.adc_read_count % NO_VALUES_TO_SAVE] = temp_mutex_wait;
            }
        }

        int64_t end_time = esp_timer_get_time();
        // Record ADC timing
        int64_t adc_elapsed = end_time - start;

        int64_t mw_start2 = esp_timer_get_time();
        if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            uint32_t sample_idx = metrics.adc_read_count % NO_VALUES_TO_SAVE;
            int64_t metrics_mutex_wait = esp_timer_get_time() - mw_start2;
            metrics.reader_metrics_mutex_wait_us[sample_idx] = metrics_mutex_wait;

            metrics.total_adc_read_time_us += adc_elapsed;
            metrics.adc_times[sample_idx] = adc_elapsed;
            metrics.free_heap_bytes[sample_idx] = esp_get_free_heap_size();
            metrics.temperature_c[sample_idx] = temperature;

            metrics.adc_read_count++;

            // E2E: Only trigger if we're waiting AND temp rose above threshold
            if (metrics.waiting_for_rise &&
                (temperature - metrics.temp_at_command) > E2E_THRESHOLD)
            {
                int64_t e2e_delay = end_time - metrics.command_send_time_us;
                metrics.total_e2e_delay_us += e2e_delay;
                metrics.e2e_times[metrics.e2e_delay_count % NO_VALUES_TO_SAVE] = e2e_delay;
                metrics.e2e_delay_count++;
                metrics.waiting_for_rise = false;
            }
            xSemaphoreGive(metrics_mutex);
        }
        if (SYSTEM_LOGGING_ENABLED)
        {
            ESP_LOGI(TAG, "Raw: %d | Volts: %d mV | Temp: %.1f C | ADC time: %lld us",
                     adc_raw, voltage_mv, temperature, adc_elapsed);
        }

        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void metrics_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        // Simple log of vital stats
        if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(100)))
        {
            ESP_LOGI(METRICS_TAG, "Loops: %lu | Avg Loop: %lld us | E2E Avg: %lld us",
                     metrics.loop_iterations,
                     metrics.loop_iterations ? metrics.total_loop_time_us / metrics.loop_iterations : 0,
                     metrics.e2e_delay_count ? metrics.total_e2e_delay_us / metrics.e2e_delay_count : 0);
            xSemaphoreGive(metrics_mutex);
        }
    }
}

void calculate_cpu_usage(void *args)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(CPU_MEASUREMENT_INTERVAL_MS));
        uint32_t total_runtime;
        UBaseType_t num_tasks = uxTaskGetSystemState(task_status_array, MAX_TASKS, &total_runtime);

        uint32_t delta_total = total_runtime - prev_total_runtime;
        prev_total_runtime = total_runtime;

        float core_usage_0, core_usage_1, overall_usage;
        _calculate_cpu_metrics(task_status_array, num_tasks, delta_total,
                               &core_usage_0, &core_usage_1, &overall_usage);
        if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {

            metrics.cpu_stats[metrics.cpu_log_count % NO_VALUES_TO_SAVE].core_0_usage = core_usage_0;
            metrics.cpu_stats[metrics.cpu_log_count % NO_VALUES_TO_SAVE].core_1_usage = core_usage_1;
            metrics.cpu_stats[metrics.cpu_log_count % NO_VALUES_TO_SAVE].overall_cpu_usage = overall_usage;
            metrics.cpu_log_count++;

            xSemaphoreGive(metrics_mutex);
        }
    }
}

void app_main(void)
{
    temp_mutex = xSemaphoreCreateMutex();
    init_metrics();
    init_heater_dac();

    // --- CPU MEASUREMENT INIT ---
#if ENABLE_CPU_SAMPLER
    init_cpu_measurement();

    // run cpu profiling task - Pinned to Core 1
    xTaskCreatePinnedToCore(calculate_cpu_usage, "CPU Usage", 4096, NULL, 3, NULL, 1);
#endif


    // 2. Start Metrics Logging - Pinned to Core 1
    if (LOG_METRICS_ENABLED)
    {
        xTaskCreatePinnedToCore(metrics_task, "Metrics", 4096, NULL, 2, NULL, 1);
    }

    ESP_LOGI(TAG, "Execution mode: %s", USE_AOT ? "AOT" : "Interpreter");
    ESP_LOGI(TAG, "Control algorithm core: %d", CONTROL_ALGORITHM_CORE);
    ESP_LOGI(TAG, "Initialization Complete. Starting Control Algorithm...");

    // Reset simulator to ambient temperature before starting
    reset_simulator();

    // Log initial memory state before WASM loads
    ESP_LOGI(METRICS_TAG, "=== PRE-WASM MEMORY STATE ===");
    log_memory_stats();

    // 1. Start the Background Reader Task (Hardware Interface) - Pinned to Core 1
    xTaskCreatePinnedToCore(reader_task, "ADC Reader", 4096, NULL, 5, NULL, 1);
    // Configure pthread to pin the control algorithm to the selected core
    esp_pthread_cfg_t pthread_cfg = esp_pthread_get_default_config();
    pthread_cfg.stack_size = 24 * 1024;
    pthread_cfg.pin_to_core = CONTROL_ALGORITHM_CORE;
    pthread_cfg.prio = 5;
    pthread_cfg.thread_name = "WASM Runtime";
    esp_pthread_set_cfg(&pthread_cfg);

    // Select container list based on TEST_MODE
#if TEST_MODE == 0
    static const char *containers[] = {
        "/spiffs/e2e_step_response" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 1
    static const char *containers[] = {
        "/spiffs/bang_bang_finite" WASM_EXT,
        "/spiffs/pid_finite" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 2
    ESP_LOGW(TAG, ">>> FAULT TEST: OOB memory access <<<");
    static const char *containers[] = {
        "/spiffs/fault_null_ptr" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 3
    ESP_LOGW(TAG, ">>> FAULT TEST: Stack overflow <<<");
    static const char *containers[] = {
        "/spiffs/fault_overflow" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 4
    ESP_LOGW(TAG, ">>> FAULT TEST: Infinite loop <<<");
    static const char *containers[] = {
        "/spiffs/fault_infinite_loop" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 5
    ESP_LOGW(TAG, ">>> TEST MODE: Bang-Bang finite run <<<");
    static const char *containers[] = {
        "/spiffs/bang_bang_finite" WASM_EXT,
        NULL
    };
#elif TEST_MODE == 6
    ESP_LOGW(TAG, ">>> TEST MODE: PID finite run <<<");
    static const char *containers[] = {
        "/spiffs/pid_finite" WASM_EXT,
        NULL
    };
#endif

#if IS_FAULT_TEST_MODE
    // Start watchdog task on Core 1 for fault detection
    xTaskCreatePinnedToCore(wasm_watchdog_task, "WASM WDT", 4096, NULL, 4, NULL, 1);
#endif

    // Create WASM thread using pthread (required by WAMR)
    pthread_t wasm_thread;
    int res = pthread_create(&wasm_thread, NULL, wasm_thread_entry, (void *)containers);
    if (res != 0)
    {
        ESP_LOGE(TAG, "Failed to create WASM pthread: %d", res);
    }
    else
    {
        pthread_join(wasm_thread, NULL);
    }
}
