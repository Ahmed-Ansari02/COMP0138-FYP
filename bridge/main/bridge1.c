#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_timer.h"
#include "simulation_data_packet.h"

#define TAG "BRIDGE"

// --- PHYSICS CONSTANTS (from sim.py) ---
#define AMBIENT_TEMP      25.0f    // Room temp (C)
#define MAX_HEATER_TEMP   200.0f   // Max temp if heater stays on forever
#define HEATING_RATE      0.8f     // How fast it gains heat (deg/tick)
#define COOLING_RATE      0.02f    // How fast it loses heat to environment
#define THERMAL_MASS      0.95f    // Inertia (Higher = Slower/Smoother changes)
#define SIMULATION_TICK_MS 50      // 20Hz simulation rate

// --- GLOBAL STATE ---
static float current_temp = AMBIENT_TEMP;
static float heater_cmd = 0.0f;   // 0.0 (OFF) to 1.0 (ON)
static uint32_t start_time_ms = 0;

// Controller MAC address
uint8_t controller_mac[] = {0x08, 0x3a, 0xf2, 0x47, 0x54, 0x5c};

// Mutex for thread-safe access to heater_cmd
static SemaphoreHandle_t heater_mutex = NULL;

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

// Generate random float in range [min, max]
static float random_float(float min, float max)
{
    uint32_t rand_val = esp_random();
    float normalized = (float)rand_val / (float)UINT32_MAX;
    return min + normalized * (max - min);
}

// Physics simulation task - runs at 20Hz
static void physics_simulation_task(void *pvParameters)
{
    start_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    
    while (1)
    {
        // Get current heater command (thread-safe)
        float local_heater_cmd;
        if (xSemaphoreTake(heater_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            local_heater_cmd = heater_cmd;
            xSemaphoreGive(heater_mutex);
        }
        else
        {
            local_heater_cmd = 0.0f; // Default if mutex fails
        }

        // UPDATE PHYSICS (Newton's Law of Cooling)
        // Energy In: Heater Power
        float energy_in = local_heater_cmd * HEATING_RATE;
        // Energy Out: Difference between object and room temp
        float energy_out = (current_temp - AMBIENT_TEMP) * COOLING_RATE;
        
        // Apply Thermal Mass (Smoothing/Lag)
        float target_next_temp = current_temp + energy_in - energy_out;
        current_temp = (current_temp * THERMAL_MASS) + (target_next_temp * (1.0f - THERMAL_MASS));

        // Add sensor noise for realistic PID testing
        float noise = random_float(-0.3f, 0.3f);
        float simulated_reading = current_temp + noise;
        
        // Create and send sensor packet to controller
        // Device 0 (Bridge/Simulator), Sensor 1 (Temp)
        uint32_t timestamp = (uint32_t)(esp_timer_get_time() / 1000) - start_time_ms;
        SimPacket sensor_packet = {
            .device_id = 0,
            .id = 1,
            .value = simulated_reading,
            .counter = timestamp
        };
        
        esp_err_t result = esp_now_send(controller_mac, (uint8_t*)&sensor_packet, sizeof(SimPacket));
        if (result != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to send sensor data: %s", esp_err_to_name(result));
        }
        
        // Simulation Tick Rate (20Hz = 50ms)
        vTaskDelay(pdMS_TO_TICKS(SIMULATION_TICK_MS));
    }
}

// Callback when receiving data from controller via ESP-NOW
void onReceiveData(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len)
{
    if (len == sizeof(SimPacket))
    {
        SimPacket *packet = (SimPacket*)data;
        
        // If packet comes from Controller (Dev ID 1) and is for Heater (ID 1)
        if (packet->device_id == 1 && packet->id == 1)
        {
            float new_cmd = packet->value;
            // Clamp heater command to 0-1 range
            if (new_cmd <= 0.0f) new_cmd = 0.0f;
            if (new_cmd >= 1.0f) new_cmd = 1.0f;
            
            // Update heater command (thread-safe)
            if (xSemaphoreTake(heater_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
            {
                heater_cmd = new_cmd;
                xSemaphoreGive(heater_mutex);
            }
            
            ESP_LOGI(TAG, "Command Recv: Heater Power %.0f%%", new_cmd * 100.0f);
        }
    }
}

void app_main(void)
{
    // Create mutex for heater_cmd access
    heater_mutex = xSemaphoreCreateMutex();
    if (heater_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    // Initialize ESP-NOW
    esp_now_wifi_init();
    add_peer(controller_mac);
    esp_now_register_recv_cb(onReceiveData);
    
    ESP_LOGI(TAG, "Bridge started - Physics simulation running on ESP32");
    ESP_LOGI(TAG, "Ambient: %.1fC, Heating Rate: %.2f, Cooling Rate: %.2f", 
             AMBIENT_TEMP, HEATING_RATE, COOLING_RATE);
    
    // Start physics simulation task
    xTaskCreate(physics_simulation_task, "physics_sim", 4096, NULL, 5, NULL);
}