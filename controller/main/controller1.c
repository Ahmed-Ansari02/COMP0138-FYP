#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "simulation_data_packet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "wasm_export.h"

#define TAG "CONTROLLER"
#define GLOBAL_HEAP_SIZE (50 * 1024)

uint8_t bridge_mac[] = {0x08, 0x3a, 0xf2, 0x45, 0xae, 0xac};

// --- STATE VARIABLES (shared with WASM) ---
static float current_temp = 25.0f; // Current temperature from bridge
static float heater_cmd = 0.0f;    // 0.0 = OFF, 1.0 = ON
static SemaphoreHandle_t temp_mutex = NULL;

// ============================================================================
// NATIVE FUNCTIONS (Exposed to WASM)
// ============================================================================

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

// Set heater command (0.0 = OFF, 1.0 = ON)
void host_set_heater(wasm_exec_env_t exec_env, float value)
{
    // Clamp value to 0-1 range
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        heater_cmd = value;
        xSemaphoreGive(temp_mutex);
    }
    ESP_LOGI(TAG, "WASM set heater to %.0f%%", value * 100.0f);
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
        ESP_LOGI("WASM", "%s", message);
    }
}

// Native symbol registration
static NativeSymbol native_symbols[] = {
    {"host_get_temperature", host_get_temperature, "()f", NULL},
    {"host_set_heater", host_set_heater, "(f)", NULL},
    {"host_delay", host_delay, "(i)", NULL},
    {"host_log", host_log, "($)", NULL},
};

// ============================================================================
// SPIFFS & WASM LOADER
// ============================================================================

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
    module_inst = wasm_runtime_instantiate(module, 8 * 1024, 8 * 1024, error_buf, sizeof(error_buf));
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

    // Store global instance
    global_module_inst = module_inst;

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
    global_module_inst = NULL;
    wasm_runtime_destroy_exec_env(exec_env);
    wasm_runtime_deinstantiate(module_inst);
    wasm_runtime_unload(module);
}

// ============================================================================
// ESP-NOW COMMUNICATION
// ============================================================================

static void esp_now_wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (esp_now_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW Init Failed");
        return;
    }
}

static void add_peer(uint8_t mac_addr[6])
{
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac_addr, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add peer");
        return;
    }
}

// ESP-NOW receive callback - updates current temperature
void onReceiveData(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    if (len == sizeof(SimPacket))
    {
        SimPacket *p = (SimPacket *)data;

        // Check if this is a temperature sensor reading from the bridge (device_id=0, id=1)
        if (p->device_id == 0 && p->id == 1)
        {
            // Update current temperature (thread-safe)
            if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                current_temp = p->value;
                xSemaphoreGive(temp_mutex);
            }
        }
    }
}

// ============================================================================
// SENDER TASK - Continuously sends heater commands to bridge
// ============================================================================

void sender_task(void *arg)
{
    int32_t packet_counter = 0;
    const int SEND_INTERVAL_MS = 100; // 10Hz

    while (1)
    {
        float cmd_to_send;
        if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            cmd_to_send = heater_cmd;
            xSemaphoreGive(temp_mutex);
        }
        else
        {
            cmd_to_send = 0.0f;
        }

        SimPacket packet = {
            .device_id = 1,
            .id = 1,
            .value = cmd_to_send,
            .counter = packet_counter++};

        esp_now_send(bridge_mac, (uint8_t *)&packet, sizeof(packet));
        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
    }
}

// ============================================================================
// WASM THREAD ENTRY (required for WAMR pthread compatibility)
// ============================================================================

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

    // Initialize WAMR
    static char global_heap_buf[GLOBAL_HEAP_SIZE];

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args))
    {
        ESP_LOGE(TAG, "WAMR Init Failed");
        return NULL;
    }

    // Register native functions
    wasm_runtime_register_natives("env", native_symbols, sizeof(native_symbols) / sizeof(NativeSymbol));

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Loading WASM Controller: %s", file_list[0].filename);
    ESP_LOGI(TAG, "================================================");

    run_wasm(wasm_file, file_size);
    free(wasm_file);

    return NULL;
}

void app_main(void)
{
    // Create mutex for thread-safe access
    temp_mutex = xSemaphoreCreateMutex();
    if (temp_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    // Initialize ESP-NOW
    esp_now_wifi_init();
    add_peer(bridge_mac);
    esp_now_register_recv_cb(onReceiveData);

    ESP_LOGI(TAG, "Controller Started - WASM Control Mode");

    // Start sender task (sends heater commands to bridge)
    xTaskCreate(sender_task, "sender_task", 4096, NULL, 5, NULL);

    // Start WASM in a pthread (required for WAMR)
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