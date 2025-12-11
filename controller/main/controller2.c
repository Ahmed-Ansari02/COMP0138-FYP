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
static float current_temp = 25.0f;     // Current temperature from bridge
static float heater_cmd = 0.0f;        // 0.0 = OFF, 1.0 = ON
static SemaphoreHandle_t temp_mutex = NULL;

// --- WASM State ---
static wasm_module_inst_t global_module_inst = NULL;

// --- File Storage for WASM files ---
#define MAX_FILES 10
#define MAX_FILENAME_LEN 64

typedef struct {
    char filename[MAX_FILENAME_LEN];
} file_entry_t;

static file_entry_t file_list[MAX_FILES];
static int file_count = 0;

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
        if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW Init Failed");
        return;
    }
}

static void add_peer(uint8_t mac_addr[6]) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac_addr, 6);
    peerInfo.channel = 0;  
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK){
        ESP_LOGE(TAG, "Failed to add peer");
        return;
    }
}

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
                
                // Bang-bang control with hysteresis
                if (current_temp < (target_temp - hysteresis))
                {
                    heater_cmd = 1.0f;  // Turn ON heater
                }
                else if (current_temp > (target_temp + hysteresis))
                {
                    heater_cmd = 0.0f;  // Turn OFF heater
                }
                // else: keep current state (within hysteresis band)
                
                xSemaphoreGive(temp_mutex);
            }
            
            ESP_LOGI(TAG, "Temp: %.2f°C | Target: %.1f°C | Heater: %s",
                     p->value, target_temp, heater_cmd > 0.5f ? "ON" : "OFF");
        }
    }
}


void app_main(void) {
    // Create mutex for thread-safe access
    temp_mutex = xSemaphoreCreateMutex();
    if (temp_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    // 1. Init Wi-Fi
    esp_now_wifi_init();

    // 2. Add Bridge as a Peer
    add_peer(bridge_mac);
    esp_now_register_recv_cb(onReceiveData);

    ESP_LOGI(TAG, "Controller Started - Bang-Bang Temperature Control");
    ESP_LOGI(TAG, "Target: %.1f°C | Hysteresis: ±%.1f°C", target_temp, hysteresis);

    // Packet counter
    int32_t packet_counter = 0;
    
    // Timing Variables
    const int SEND_INTERVAL_MS = 100;   // Send data every 100ms (10Hz)

    while (1) {
        // Get current heater command (thread-safe)
        float cmd_to_send;
        if (xSemaphoreTake(temp_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            cmd_to_send = heater_cmd;
            xSemaphoreGive(temp_mutex);
        }
        else
        {
            cmd_to_send = 0.0f;  // Default to OFF if mutex fails
        }

        // Prepare Packet (Transport Layer)
        SimPacket packet;
        packet.device_id = 1;       
        packet.id = 1;              
        packet.value = cmd_to_send;  // Send heater command based on temperature control
        packet.counter = packet_counter++;

        // Send Packet to Bridge
        esp_now_send(bridge_mac, (uint8_t *) &packet, sizeof(packet));

        // Wait before sending again (10Hz)
        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS)); 
    }
}