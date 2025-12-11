/* controller/main/main.c */
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "simulation_data_packet.h"
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

#define TAG "CONTROLLER"

#define PIN_ADC_CHAN ADC_CHANNEL_4 // GPIO 32 

#define PIN_HEATER_OUT 26

// Define the attenuation (DB_12 allows reading up to approx 3.1V - 3.3V)
#define ADC_ATTEN ADC_ATTEN_DB_12

// --- STATE VARIABLES (shared with WASM) ---
static float current_temp = 25.0f; // Current temperature from bridge
static float heater_cmd = 0.0f;    // 0.0 = OFF, 1.0 = ON
static SemaphoreHandle_t temp_mutex = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;

// --- PWM Configuration ---
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          26 // Pin connected to Simulator
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Resolution (0-8191)
#define LEDC_FREQUENCY          1000 // 1kHz Frequency (1ms period)

// ============================================================================
// NATIVE FUNCTIONS (Exposed to WASM)
// ============================================================================

void init_heater_pwm() {
    // Timer Config
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY, 
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Channel Config
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Start OFF
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

// Get current temperature reading from the bridge
float host_get_temperature(wasm_exec_env_t exec_env)
{
    float temp = 25.0f;
    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        temp = current_temp;
        xSemaphoreGive(temp_mutex);
    }
    return temp;
}

// Set heater command (0= OFF, 1 = ON)
void host_set_heater(wasm_exec_env_t exec_env, int value)
{
    // Clamp value to 0-1 range
    gpio_set_level(PIN_HEATER_OUT, value);
}

// Delay function for WASM
void host_delay(wasm_exec_env_t exec_env, int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Log function for WASM (prints to ESP32 console)
void host_log(wasm_exec_env_t exec_env, const char *message)
{
    if (message)
    {
        ESP_LOGI(TAG, "WASM: %s", message);
    }
}

// Native symbol registration
static NativeSymbol native_symbols[] = {
    {"host_get_temperature", host_get_temperature, "()f", NULL},
    {"host_set_heater", host_set_heater, "(i)", NULL},
    {"host_delay", host_delay, "(i)", NULL},
    {"host_log", host_log, "($)", NULL},
};

void run_wasm(uint8_t *buffer, uint32_t size)
{
    char error_buf[128];
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;

    // Load module
    module = wasm_runtime_load(buffer, size, error_buf, sizeof(error_buf));
    if (!module)
    {
        ESP_LOGE(TAG, "WASM load failed: %s", error_buf);
        return;
    }

    // Instantiate module
    // stack_size: WASM operand stack size
    // heap_size: WASM app heap size (for malloc in WASM)
    // Note: Linear memory is separate and defined in the WASM module itself
    module_inst = wasm_runtime_instantiate(module, 16 * 1024, 16 * 1024, error_buf, sizeof(error_buf));
    if (!module_inst)
    {
        ESP_LOGE(TAG, "WASM instantiation failed: %s", error_buf);
        wasm_runtime_unload(module);
        return;
    }

    // Create execution environment
    exec_env = wasm_runtime_create_exec_env(module_inst, 8 * 1024);
    if (!exec_env)
    {
        ESP_LOGE(TAG, "Exec env creation failed");
        wasm_runtime_deinstantiate(module_inst);
        wasm_runtime_unload(module);
        return;
    }

    ESP_LOGI(TAG, "Starting WASM Control Module...");

    // Look for main function
    wasm_function_inst_t func = wasm_runtime_lookup_function(module_inst, "main");

    if (func)
    {
        uint32_t args[2] = {0, 0}; // argc, argv
        if (!wasm_runtime_call_wasm(exec_env, func, 2, args))
        {
            const char *exception = wasm_runtime_get_exception(module_inst);
            if (exception && strstr(exception, "terminated"))
            {
                ESP_LOGW(TAG, "WASM execution terminated");
            }
            else
            {
                ESP_LOGE(TAG, "WASM exception: %s", exception ? exception : "unknown");
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
    }

    // Cleanup
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

uint8_t *load_wasm_from_spiffs(const char *filename, uint32_t *size)
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

void *wasm_thread_entry(void *arg)
{
    // Setup SPIFFS
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

    // Register native functions
    wasm_runtime_register_natives("env", native_symbols, sizeof(native_symbols) / sizeof(NativeSymbol));

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Loading WASM container from SPIFFS...");
    ESP_LOGI(TAG, "================================================");

    uint32_t file_size = 0;
    uint8_t *wasm_file = load_wasm_from_spiffs("/spiffs/controller.wasm", &file_size);
    if (!wasm_file)
    {
        ESP_LOGE(TAG, "Failed to load WASM file from SPIFFS");
    }
    run_wasm(wasm_file, file_size);
    free(wasm_file);
    return NULL;
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
    init_adc();
    calibrate_adc();
    while (1)
    {
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, PIN_ADC_CHAN, &adc_raw));

        // --- B. Convert to Calibrated Voltage ---
        int voltage_mv = 0;

        // This function converts the raw "S-curve" to linear Millivolts
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv));

        // --- C. Convert Voltage to Temperature ---
        // NEW LOGIC: We map 3300mV (3.3V) to 100 degrees Celsius
        float temperature = ((float)voltage_mv / 3300.0f) * 100.0f;

        if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            current_temp = temperature;
            xSemaphoreGive(temp_mutex);
        }

        // Print both Raw and Calibrated Voltage for debugging
        ESP_LOGI(TAG, "Raw: %d | Volts: %d mV | Temp: %.1f C | Cmd: %d",
                 adc_raw, voltage_mv, temperature, heater_cmd);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{

    temp_mutex = xSemaphoreCreateMutex();
    if (temp_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    gpio_set_direction(PIN_HEATER_OUT, GPIO_MODE_OUTPUT);
    init_heater_pwm();
    xTaskCreate(reader_task, "ADC Reader Task", 4096, NULL, 5, NULL);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 24 * 1024);

    int res = pthread_create(&t, &attr, wasm_thread_entry, NULL);
    if (res != 0)
    {
        ESP_LOGE(TAG, "Failed to create WASM pthread: %d", res);
    }
    else
    {
        pthread_join(t, NULL);
    }
}