/* controller/main/main.c */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

// --- NEW HEADERS FOR CALIBRATION ---
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// --- PINS for TTGO T-Display ---
// GPIO 33 is available on the header and works with ADC1
#define PIN_ADC_CHAN    ADC_CHANNEL_4
// GPIO 26 is available on the header (Also DAC2, but used here as Digital Out)
#define PIN_HEATER_OUT  26            

// Define the attenuation (DB_12 allows reading up to approx 3.1V - 3.3V)
#define ADC_ATTEN       ADC_ATTEN_DB_12

void app_main(void)
{
    // ------------- 1. Setup ADC (Hardware) -------------
    adc_oneshot_unit_handle_t adc_handle;
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

    // ------------- 2. Setup Calibration (Software) -------------
    // This loads the factory reference voltage from the chip's eFuse
    ESP_LOGI("CTRL", "Setting up calibration scheme...");
    
    adc_cali_handle_t cali_handle = NULL;
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    // Check if calibration is successful
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (ret == ESP_OK) {
        ESP_LOGI("CTRL", "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW("CTRL", "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE("CTRL", "Invalid arg or no memory");
    }

    // ------------- 3. Setup Digital Output -------------
    gpio_set_direction(PIN_HEATER_OUT, GPIO_MODE_OUTPUT);

    // Debug: Convert ADC channel to GPIO number for verification
    int io_num;
    ESP_ERROR_CHECK(adc_oneshot_channel_to_io(ADC_UNIT_1, PIN_ADC_CHAN, &io_num));
    ESP_LOGI("CTRL", "Controller Started. Pin %d (ADC) <-> Pin %d (Out)", io_num, PIN_HEATER_OUT);

    while (1) {
        // --- A. Read Raw Data ---
        int adc_raw;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, PIN_ADC_CHAN, &adc_raw));

        // --- B. Convert to Calibrated Voltage ---
        int voltage_mv = 0;
        if (cali_handle) {
            // This function converts the raw "S-curve" to linear Millivolts
            ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, adc_raw, &voltage_mv));
        } else {
            // Fallback if calibration failed: rough estimation
            // 2500mV is roughly the reference for some chips, but this is inaccurate
            voltage_mv = adc_raw * 2500 / 4095; 
        }

        // --- C. Convert Voltage to Temperature ---
        // PREVIOUS LOGIC: float temperature = ((float)adc_raw / 4095.0f) * 100.0f;
        // NEW LOGIC: We map 3300mV (3.3V) to 100 degrees Celsius
        float temperature = ((float)voltage_mv / 3300.0f) * 100.0f;

        // --- D. Logic (Thermostat) ---
        int heater_cmd = 0;
        if (temperature < 50.0f) {
            heater_cmd = 1; // Turn ON
        } else {
            heater_cmd = 0; // Turn OFF
        }

        // --- E. Write to Output ---
        gpio_set_level(PIN_HEATER_OUT, heater_cmd);

        // Print both Raw and Calibrated Voltage for debugging
        ESP_LOGI("CTRL", "Raw: %d | Volts: %d mV | Temp: %.1f C | Cmd: %d", 
                 adc_raw, voltage_mv, temperature, heater_cmd);

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Teardown (Unreachable in this while(1) loop, but good practice)
    if (cali_handle) {
        adc_cali_delete_scheme_line_fitting(cali_handle);
    }
    adc_oneshot_del_unit(adc_handle);
}