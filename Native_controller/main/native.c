#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/dac_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_freertos_hooks.h" // CPU Usage

#define TAG "NATIVE_CONTROLLER"
#define METRICS_TAG "METRICS"
#define SYSTEM_LOGGING_ENABLED 0
#define LOG_METRICS_ENABLED 0
#define RECORD_METRICS 1
#define PRINT_TO_CSV 1
#define NO_VALUES_TO_SAVE 1000
#define MAX_TASKS 20
#define CPU_MEASUREMENT_INTERVAL_MS 10
#define E2E_THRESHOLD 0.5f
#define MAX_SWAPS 10
#define CONTROL_PERIOD_MS 100
#define TARGET_TEMP 40.0f
#define CONTROL_ALGORITHM_CORE 0
#define HOTSWAP_ITERATIONS_PER_ALGO 300
#define CONTROL_ALGO_ITERATIONS 1000
#define FAULT_PRE_ITERATIONS 10

#if (CONTROL_ALGORITHM_CORE != 0) && (CONTROL_ALGORITHM_CORE != 1)
#error "CONTROL_ALGORITHM_CORE must be 0 or 1"
#endif

// Test mode selector:
//   0 = E2E step response (default)
//   1 = Hot-swap test (bang-bang -> PID)
//   2 = Fault: NULL pointer dereference
//   3 = Fault: Stack overflow
//   4 = Fault: Infinite loop
//   5 = Bang-bang finite run (CSV then exit)
//   6 = PID finite run (CSV then exit)
#define TEST_MODE 5

// ============================================================================
// HARDWARE DEFINITIONS
// ============================================================================
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

#if TEST_MODE == 1
// Hot-swap metrics
typedef struct
{
    int64_t swap_latency_us[MAX_SWAPS];   // Time to switch between algorithms
    float temp_at_swap[MAX_SWAPS];         // Temperature when swap initiated
    float temp_after_swap[MAX_SWAPS];      // Temperature 1s after swap
    int64_t swap_timestamp_us[MAX_SWAPS];  // Absolute time of each swap
    uint32_t swap_count;
} swap_metrics_t;

static swap_metrics_t swap_metrics = {0};
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

// ============================================================================
// HARDWARE ABSTRACTION LAYERS (Adapted for Native use with Metrics)
// ============================================================================

void init_adc()
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = 0,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, PIN_ADC_CHAN, &config));
}

void init_heater_dac()
{
    dac_oneshot_config_t cfg = {.chan_id = DAC_CHAN_0}; // GPIO25/26
    dac_oneshot_new_channel(&cfg, &heater_dac);
}

void calibrate_adc()
{
    ESP_LOGI("CTRL", "Setting up calibration scheme...");
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "Calibration Success");
}

// --- SIMULATOR RESET ---
// Pulse GPIO13 high to tell the simulator to reset temperature to ambient.
// Holds the signal for 200ms then waits 2s for the simulator to settle.
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
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(RESET_SIGNAL_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Simulator reset complete");
}

// --- NATIVE WRAPPERS FOR ALGORITHMS ---

void record_temp_get_time(int64_t elapsed)
{
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.temperature_get_times[metrics.temperature_get_count % NO_VALUES_TO_SAVE] = elapsed;
        metrics.temperature_get_count++;
        metrics.total_temperature_get_time_us += elapsed;
        xSemaphoreGive(metrics_mutex);
    }
}

void record_heater_set_time(int64_t elapsed)
{
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.heater_set_times[metrics.heater_set_count % NO_VALUES_TO_SAVE] = elapsed;
        metrics.total_heater_set_time_us += elapsed;
        metrics.heater_set_count++;
        xSemaphoreGive(metrics_mutex);
    }
}

float native_get_temperature(void)
{
    float temp = 25.0f;
    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        temp = current_temp;
        xSemaphoreGive(temp_mutex);
    }
    return temp;
}

void native_set_heater(float value)
{
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

void native_delay(int ms)
{
    // Loop Metrics
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

void native_log(const char *msg)
{
    ESP_LOGI("ALGO", "%s", msg);
}

// ============================================================================
// CONTROL ALGORITHMS
// ============================================================================

// --- HELPER FOR PID ---
static float clamp(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}


// --- ALGORITHM 1: PID CONTROLLER ---
void algorithm_pid(void)
{
    native_log("Starting PID Controller...");
    native_log("Target: 35C");

    const float KP = 1.2f;
    const float KI = 0.08f;
    const float KD = 0.25f;
    const float INTEGRAL_MIN = -20.0f;
    const float INTEGRAL_MAX = 20.0f;

    const float dt = CONTROL_PERIOD_MS / 1000.0f;
    float integral = 0.0f;
    float prev_error = 0.0f;
    int log_counter = 0;

    while (1)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        float error = TARGET_TEMP - temp;

        integral += error * dt;
        integral = clamp(integral, INTEGRAL_MIN, INTEGRAL_MAX);

        float derivative = (error - prev_error) / dt;
        float command = (KP * error) + (KI * integral) + (KD * derivative);
        command = clamp(command, 0.0f, 1.0f);

        t_start = esp_timer_get_time();
        native_set_heater(command);
        record_heater_set_time(esp_timer_get_time() - t_start);

        if (log_counter++ >= 20)
        {
            log_counter = 0;
            native_log("PID Update");
        }
        prev_error = error;
        native_delay(CONTROL_PERIOD_MS);
    }
}

// --- ALGORITHM 2: BANG-BANG (HYSTERESIS) ---
void algorithm_bang_bang(void)
{
    native_log("Starting Bang-Bang Controller...");

    const float DEVIATION = 1.0f;

    while (1)
    {
        int64_t t_start = esp_timer_get_time();
        float current_temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (current_temp < (TARGET_TEMP - DEVIATION))
        {
            native_set_heater(1.0f);
        }
        else if (current_temp > (TARGET_TEMP + DEVIATION))
        {
            native_set_heater(0.0f);
        }
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }
}

void algorithm_bang_bang_finite(void)
{
    native_log("Starting Bang-Bang Controller (finite run)...");

    const float DEVIATION = 1.0f;

    for (int i = 0; i < CONTROL_ALGO_ITERATIONS; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (temp < (TARGET_TEMP - DEVIATION))
            native_set_heater(1.0f);
        else if (temp > (TARGET_TEMP + DEVIATION))
            native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }

    native_log("Bang-Bang finite run complete");
}

// --- ALGORITHM 3: SIMPLE TOGGLE (TEST) ---
void algorithm_test_end_end(void)
{
    native_log("Starting Simple Toggle Test...");

    const int ON_PERIOD = 10;
    const int OFF_PERIOD = 10;
    int counter = 5000;

    while (counter > 0)
    {
        int64_t t_start = esp_timer_get_time();
        native_get_temperature(); // Read just to keep metrics alive
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        native_set_heater(1.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(ON_PERIOD);

        t_start = esp_timer_get_time();
        native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(OFF_PERIOD);

        counter--;
    }
    native_log("Toggle Test Complete");
}

// --- ALGORITHM 4: STEP-RESPONSE E2E LATENCY ---
void algorithm_e2e_step_response(void)
{
    native_log("Starting Step-Response E2E Test...");

    const int PERIOD_MS = 10;           // 100Hz metrics sampling
    const int STABILIZE_ITERS = 500;    // 5s initial stabilization (heater OFF)
    const int HEAT_ITERS = 300;         // 3s heater ON per step
    const int COOL_ITERS = 1200;        // 12s heater OFF per step (cooldown)
    const int NUM_STEPS = 6;            // 6 step-response measurements

    // Phase 1: Stabilize at ambient (heater OFF)
    native_set_heater(0.0f);
    for (int i = 0; i < STABILIZE_ITERS; i++)
    {
        int64_t t = esp_timer_get_time();
        native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t);
        native_delay(PERIOD_MS);
    }

    // Phase 2: Repeated step responses
    for (int step = 0; step < NUM_STEPS; step++)
    {
        // Step ON — this arms E2E detection in reader_task
        int64_t t = esp_timer_get_time();
        native_set_heater(1.0f);
        record_heater_set_time(esp_timer_get_time() - t);

        // Hold ON, keep collecting metrics
        for (int i = 0; i < HEAT_ITERS; i++)
        {
            t = esp_timer_get_time();
            native_get_temperature();
            record_temp_get_time(esp_timer_get_time() - t);
            native_delay(PERIOD_MS);
        }

        // Step OFF
        t = esp_timer_get_time();
        native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t);

        // Cooldown, keep collecting metrics
        for (int i = 0; i < COOL_ITERS; i++)
        {
            t = esp_timer_get_time();
            native_get_temperature();
            record_temp_get_time(esp_timer_get_time() - t);
            native_delay(PERIOD_MS);
        }
    }

    native_log("Step-Response E2E Test Complete");
}

void algorithm_pid_finite(void)
{
    native_log("Starting PID Controller (finite run)...");

    const float KP = 1.2f;
    const float KI = 0.08f;
    const float KD = 0.25f;
    const float INTEGRAL_MIN = -20.0f;
    const float INTEGRAL_MAX = 20.0f;
    const float dt = CONTROL_PERIOD_MS / 1000.0f;
    float integral = 0.0f;
    float prev_error = 0.0f;

    for (int i = 0; i < CONTROL_ALGO_ITERATIONS; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        float error = TARGET_TEMP - temp;
        integral += error * dt;
        integral = clamp(integral, INTEGRAL_MIN, INTEGRAL_MAX);
        float derivative = (error - prev_error) / dt;
        float command = (KP * error) + (KI * integral) + (KD * derivative);
        command = clamp(command, 0.0f, 1.0f);

        t_start = esp_timer_get_time();
        native_set_heater(command);
        record_heater_set_time(esp_timer_get_time() - t_start);

        prev_error = error;
        native_delay(CONTROL_PERIOD_MS);
    }

    native_log("PID finite run complete");
}

#if TEST_MODE == 1
// --- ALGORITHM 5: HOT-SWAP TEST (Bang-Bang → PID) ---
void algorithm_hot_swap_test(void)
{
    native_log("Starting Hot-Swap Test: Bang-Bang -> PID");

    // ---- Phase 1: Bang-Bang for 30 iterations ----
    native_log("Phase 1: Bang-Bang controller");
    const float DEVIATION = 1.0f;

    for (int i = 0; i < HOTSWAP_ITERATIONS_PER_ALGO; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (temp < (TARGET_TEMP - DEVIATION))
            native_set_heater(1.0f);
        else if (temp > (TARGET_TEMP + DEVIATION))
            native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }

    // ---- Swap point: measure transition ----
    int64_t swap_start = esp_timer_get_time();
    float temp_before_swap = native_get_temperature();

    // In native, "swap" is just moving to the next function — essentially zero cost
    // We record the timestamp to measure the gap

    int64_t swap_end = esp_timer_get_time();

    if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        uint32_t idx = swap_metrics.swap_count;
        if (idx < MAX_SWAPS)
        {
            swap_metrics.swap_latency_us[idx] = swap_end - swap_start;
            swap_metrics.temp_at_swap[idx] = temp_before_swap;
            swap_metrics.swap_timestamp_us[idx] = swap_start;
            swap_metrics.swap_count++;
        }
        xSemaphoreGive(metrics_mutex);
    }

    native_log("SWAP: Bang-Bang -> PID");

    // ---- Phase 2: PID for 30 iterations ----
    native_log("Phase 2: PID controller");
    const float KP = 1.2f;
    const float KI = 0.08f;
    const float KD = 0.25f;
    const float INTEGRAL_MIN = -20.0f;
    const float INTEGRAL_MAX = 20.0f;
    const float dt = CONTROL_PERIOD_MS / 1000.0f;
    float integral = 0.0f;
    float prev_error = 0.0f;

    for (int i = 0; i < HOTSWAP_ITERATIONS_PER_ALGO; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        // Record temp_after_swap on the first PID iteration
        if (i == 0)
        {
            if (xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                uint32_t idx = swap_metrics.swap_count - 1;
                if (idx < MAX_SWAPS)
                    swap_metrics.temp_after_swap[idx] = temp;
                xSemaphoreGive(metrics_mutex);
            }
        }

        float error = TARGET_TEMP - temp;
        integral += error * dt;
        integral = clamp(integral, INTEGRAL_MIN, INTEGRAL_MAX);
        float derivative = (error - prev_error) / dt;
        float command = (KP * error) + (KI * integral) + (KD * derivative);
        command = clamp(command, 0.0f, 1.0f);

        t_start = esp_timer_get_time();
        native_set_heater(command);
        record_heater_set_time(esp_timer_get_time() - t_start);

        prev_error = error;
        native_delay(CONTROL_PERIOD_MS);
    }

    native_log("Hot-Swap Test Complete");
}
#endif

#if TEST_MODE >= 2
// ============================================================================
// FAULT INJECTION ALGORITHMS (Phase 2)
// ============================================================================

static void print_fault_csv(const char *fault_type, int iterations, float temp, uint32_t heap)
{
    printf("\n<<<FAULT_NATIVE_START>>>\n");
    printf("fault_type,pre_fault_iterations,temp_at_fault,heap_at_fault,timestamp_us\n");
    printf("%s,%d,%.2f,%lu,%lld\n", fault_type, iterations, temp, heap, esp_timer_get_time());
    printf("<<<FAULT_NATIVE_END>>>\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100)); // Ensure serial flush before crash
}

#if TEST_MODE == 2
// --- FAULT 1: NULL POINTER DEREFERENCE ---
void algorithm_fault_null_ptr(void)
{
    native_log("Starting Fault Test: NULL Pointer Dereference");
    native_log("Running 10 bang-bang iterations, then triggering fault...");

    const float DEVIATION = 1.0f;

    for (int i = 0; i < FAULT_PRE_ITERATIONS; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (temp < (TARGET_TEMP - DEVIATION))
            native_set_heater(1.0f);
        else if (temp > (TARGET_TEMP + DEVIATION))
            native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }

    // Print pre-crash CSV
    float temp = native_get_temperature();
    uint32_t heap = esp_get_free_heap_size();
    print_fault_csv("null_ptr", FAULT_PRE_ITERATIONS, temp, heap);

    // Trigger fault: NULL pointer dereference
    native_log("FAULT: Dereferencing NULL pointer NOW");
    volatile int *p = NULL;
    *p = 42;
}

#endif // TEST_MODE == 2

#if TEST_MODE == 3
// --- FAULT 2: STACK OVERFLOW (BUFFER OVERRUN) ---
void algorithm_fault_overflow(void)
{
    native_log("Starting Fault Test: Stack Overflow");
    native_log("Running 10 bang-bang iterations, then triggering fault...");

    const float DEVIATION = 1.0f;

    for (int i = 0; i < FAULT_PRE_ITERATIONS; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (temp < (TARGET_TEMP - DEVIATION))
            native_set_heater(1.0f);
        else if (temp > (TARGET_TEMP + DEVIATION))
            native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }

    // Print pre-crash CSV
    float temp = native_get_temperature();
    uint32_t heap = esp_get_free_heap_size();
    print_fault_csv("stack_overflow", FAULT_PRE_ITERATIONS, temp, heap);

    // Trigger fault: Write past a local buffer to smash the stack
    native_log("FAULT: Writing past stack buffer NOW");
    volatile char buf[64];
    for (int i = 0; i < 4096; i++)
        buf[i] = (char)i;
}

#endif // TEST_MODE == 3

#if TEST_MODE == 4
// --- FAULT 3: INFINITE LOOP (STARVES SYSTEM) ---
void algorithm_fault_infinite_loop(void)
{
    native_log("Starting Fault Test: Infinite Loop");
    native_log("Running 10 bang-bang iterations, then triggering fault...");

    const float DEVIATION = 1.0f;

    for (int i = 0; i < FAULT_PRE_ITERATIONS; i++)
    {
        int64_t t_start = esp_timer_get_time();
        float temp = native_get_temperature();
        record_temp_get_time(esp_timer_get_time() - t_start);

        t_start = esp_timer_get_time();
        if (temp < (TARGET_TEMP - DEVIATION))
            native_set_heater(1.0f);
        else if (temp > (TARGET_TEMP + DEVIATION))
            native_set_heater(0.0f);
        record_heater_set_time(esp_timer_get_time() - t_start);

        native_delay(CONTROL_PERIOD_MS);
    }

    // Print pre-crash CSV
    float temp = native_get_temperature();
    uint32_t heap = esp_get_free_heap_size();
    print_fault_csv("infinite_loop", FAULT_PRE_ITERATIONS, temp, heap);

    // Trigger fault: Infinite loop with no yields — starves Core 0
    native_log("FAULT: Entering infinite loop NOW");
    while (1)
    {
        // Tight loop, no vTaskDelay, no yields
    }
}
#endif // TEST_MODE == 4
#endif // TEST_MODE >= 2

// ============================================================================
// TASKS (Reader, Metrics, CSV)
// ============================================================================

void reader_task(void *arg)
{
    const int delay = CONTROL_PERIOD_MS;
    init_adc();
    calibrate_adc();
    while (1)
    {
        int64_t start = esp_timer_get_time();
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, PIN_ADC_CHAN, &adc_raw));

        int voltage_mv = 0;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv));

        // Assuming 3.3V = 100C scaling based on previous code context
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

            // E2E Logic
            if (metrics.waiting_for_rise && (temperature - metrics.temp_at_command) > E2E_THRESHOLD)
            {
                int64_t e2e_delay = end_time - metrics.command_send_time_us;
                metrics.total_e2e_delay_us += e2e_delay;
                metrics.e2e_times[metrics.e2e_delay_count % NO_VALUES_TO_SAVE] = e2e_delay;
                metrics.e2e_delay_count++;
                metrics.waiting_for_rise = false;
            }
            xSemaphoreGive(metrics_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
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

#if TEST_MODE == 1
    // Print swap metrics if any swaps occurred
    if (swap_metrics.swap_count > 0)
    {
        printf("\n<<<HOTSWAP_START>>>\n");
        printf("swap_index,swap_latency_us,temp_at_swap,temp_after_swap,swap_timestamp_us\n");
        for (uint32_t i = 0; i < swap_metrics.swap_count; i++)
        {
            printf("%lu,%lld,%.2f,%.2f,%lld\n",
                   i,
                   swap_metrics.swap_latency_us[i],
                   swap_metrics.temp_at_swap[i],
                   swap_metrics.temp_after_swap[i],
                   swap_metrics.swap_timestamp_us[i]);
        }
        printf("<<<HOTSWAP_END>>>\n\n");
        fflush(stdout);
    }
#endif

    xSemaphoreGive(metrics_mutex);

    ESP_LOGI(METRICS_TAG, "CSV output complete (%lu rows)", max_count);
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

void run_algorithm_task(void *arg)
{
    /**
     * Available algorithms to use with run_algorithm_task:
     *   algorithm_pid              - PID Controller (closed-loop, smooth control)
     *   algorithm_bang_bang        - Bang-Bang Controller (hysteresis, on/off)
     *   algorithm_test_end_end     - Simple Toggle Test (for end-to-end timing)
     *   algorithm_e2e_step_response- Step-Response E2E Latency (clean step measurements)
     *   algorithm_hot_swap_test    - Hot-Swap Test (Bang-Bang -> PID transition)
     *   algorithm_bang_bang_finite - Bang-Bang finite run (CSV then exit)
     *   algorithm_pid_finite       - PID finite run (CSV then exit)
     */
    void (*algorithm_func)(void) = (void (*)(void))arg;
    int64_t start_time = esp_timer_get_time();
    algorithm_func();
    int64_t end_time = esp_timer_get_time();
    if (RECORD_METRICS && xSemaphoreTake(metrics_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        metrics.total_exec_time_us += (end_time - start_time);
        metrics.exec_times[metrics.call_count % NO_VALUES_TO_SAVE] = end_time - start_time;
        metrics.call_count++;
        xSemaphoreGive(metrics_mutex);
    }

    if (PRINT_TO_CSV){
        print_metrics_csv();
    }

    print_task_runtime_stats();

    vTaskDelete(NULL);
}

// ============================================================================
// MEMORY BUS THRASHING EXPERIMENT
// Simulates WAMR interpreter memory access patterns on Core 0.
// Enable to test if active memory traffic causes Core 1 CPU overhead.
// ============================================================================
#define ENABLE_MEM_THRASH 0
#define MEM_THRASH_SIZE (64 * 1024) // Largest safe contiguous block

void mem_thrash_task(void *arg)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "mem_thrash: largest free block = %u bytes", largest);

    volatile uint8_t *buf = (volatile uint8_t *)malloc(MEM_THRASH_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "mem_thrash: malloc failed for %d bytes", MEM_THRASH_SIZE);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "mem_thrash: allocated %d bytes", MEM_THRASH_SIZE);

    // Initialize buffer
    for (int i = 0; i < MEM_THRASH_SIZE; i++) {
        buf[i] = (uint8_t)i;
    }

    uint32_t seed = 12345;
    while (1) {
        // Pseudo-random read/write across the buffer to simulate interpreter access
        for (int i = 0; i < 1000; i++) {
            seed = seed * 1103515245 + 12345; // LCG PRNG
            uint32_t idx = seed % MEM_THRASH_SIZE;
            buf[idx] = buf[idx] + 1;
        }
        // Yield briefly so we don't starve other Core 0 tasks
        vTaskDelay(1);
    }
}

// ============================================================================
// APP MAIN
// ============================================================================

void app_main(void)
{
    temp_mutex = xSemaphoreCreateMutex();
    init_metrics();
    init_heater_dac();

    // --- CPU MEASUREMENT INIT ---
    init_cpu_measurement();

    // run cpu profiling task
    xTaskCreatePinnedToCore(calculate_cpu_usage, "CPU Usage", 4096, NULL, 3, NULL, 1);

    
    // 2. Start Metrics Logging - Pinned to Core 0
    if (LOG_METRICS_ENABLED)
    {
        xTaskCreatePinnedToCore(metrics_task, "Metrics", 4096, NULL, 2, NULL, 1);
    }
    
    ESP_LOGI(TAG, "Control algorithm core: %d", CONTROL_ALGORITHM_CORE);
    ESP_LOGI(TAG, "Initialization Complete. Starting Control Algorithm...");

    // Reset simulator to ambient temperature before starting
    reset_simulator();

    // 1. Start the Background Reader Task (Hardware Interface) - Pinned to Core 1
    xTaskCreatePinnedToCore(reader_task, "ADC Reader", 4096, NULL, 5, NULL, 1);

    // Memory bus thrashing experiment - runs on Core 0 to simulate WAMR memory access
    if (ENABLE_MEM_THRASH)
    {
        xTaskCreatePinnedToCore(mem_thrash_task, "MemThrash", 4096, NULL, 1, NULL, 1);
        ESP_LOGI(TAG, "Memory thrash task started on Core 0");
    }

    // Select algorithm based on TEST_MODE
#if TEST_MODE == 0
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_e2e_step_response, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 1
    ESP_LOGW(TAG, ">>> TEST MODE: Hot-swap (Bang-Bang -> PID) <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_hot_swap_test, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 2
    ESP_LOGW(TAG, ">>> FAULT TEST: NULL pointer dereference <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_fault_null_ptr, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 3
    ESP_LOGW(TAG, ">>> FAULT TEST: Stack overflow <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_fault_overflow, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 4
    ESP_LOGW(TAG, ">>> FAULT TEST: Infinite loop <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_fault_infinite_loop, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 5
    ESP_LOGW(TAG, ">>> TEST MODE: Bang-Bang finite run <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_bang_bang_finite, 5, NULL, CONTROL_ALGORITHM_CORE);
#elif TEST_MODE == 6
    ESP_LOGW(TAG, ">>> TEST MODE: PID finite run <<<");
    xTaskCreatePinnedToCore(run_algorithm_task, "control algorithm", 4096, (void *)algorithm_pid_finite, 5, NULL, CONTROL_ALGORITHM_CORE);
#endif
}
