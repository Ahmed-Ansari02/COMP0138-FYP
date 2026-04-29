#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/dac_oneshot.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h" // For adc_continuous_io_to_channel
#include "driver/gpio.h"

// Logging behavior
#define E2E_LOGGING 0

// Reset signal from controller (active high)
#define RESET_GPIO 22
// Temperature output (Bridge → Controller)
#define PIN_DAC_CHAN DAC_CHAN_0 // GPIO 25

// Heater input (Controller → Bridge)
// Define GPIO pin instead of channel
#define HEATER_ADC_GPIO 32 // Bridge reads heater command from controller's DAC
#define ADC_ATTEN ADC_ATTEN_DB_12

// --- PHYSICS CONSTANTS ---
#define AMBIENT_TEMP 25.0f
#define MAX_TEMP 100.0f
#define HEATING_RATE 2.0f
#define COOLING_RATE 0.02f
#define THERMAL_MASS 0.7f
#define NOISE_RANGE 0.3f

static float current_temp = 25.0f;
static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle = NULL;
static adc_channel_t heater_adc_channel; // Dynamically resolved
static adc_unit_t heater_adc_unit;       // Dynamically resolved

static float random_float(float min, float max)
{
    uint32_t rand_val = esp_random();
    float normalized = (float)rand_val / (float)UINT32_MAX;
    return min + normalized * (max - min);
}

void init_heater_adc()
{
    // ------------- Resolve GPIO to ADC Channel -------------
    esp_err_t ret1 = adc_continuous_io_to_channel(HEATER_ADC_GPIO, &heater_adc_unit, &heater_adc_channel);
    if (ret1 != ESP_OK)
    {
        ESP_LOGE("SIM", "Failed to resolve GPIO %d to ADC channel: %s",
                 HEATER_ADC_GPIO, esp_err_to_name(ret1));
        return;
    }
    ESP_LOGI("SIM", "GPIO %d -> ADC%d Channel %d", HEATER_ADC_GPIO, heater_adc_unit + 1, heater_adc_channel);

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = heater_adc_unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, heater_adc_channel, &chan_cfg));

    // Calibration
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = heater_adc_unit,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &cali_handle);
    if (ret == ESP_OK)
    {
        ESP_LOGI("SIM", "ADC Calibration Success");
    }
    else
    {
        ESP_LOGW("SIM", "ADC Calibration not available");
    }

    ESP_LOGI("SIM", "Heater ADC initialized on GPIO %d", HEATER_ADC_GPIO);
}

float read_heater_power()
{
    int raw;

    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, heater_adc_channel, &raw));

    float power = (float)(raw / 4095.0f);
    ESP_LOGI("SIM", "Heater Power: %.2f%% and raw %d", power * 100, raw);
    return power;
}

void physics_simulation_loop()
{
    // Setup DAC for temperature output
    dac_oneshot_handle_t dac_handle;
    dac_oneshot_config_t dac_cfg = {.chan_id = PIN_DAC_CHAN};
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac_cfg, &dac_handle));

    while (1)
    {
        // Check for reset signal from controller
        if (gpio_get_level(RESET_GPIO) == 1) {
            current_temp = AMBIENT_TEMP;
            ESP_LOGI("SIM", "Reset signal received — temp reset to %.1fC", AMBIENT_TEMP);
        }

        // Read heater power from ADC
        float heater_power = read_heater_power();

        // Physics Simulation
        float energy_in = heater_power * HEATING_RATE;
        float energy_out = (current_temp - AMBIENT_TEMP) * COOLING_RATE;
        float target_next_temp = current_temp + energy_in - energy_out;
        current_temp = (current_temp * THERMAL_MASS) + (target_next_temp * (1.0f - THERMAL_MASS));

        if (current_temp > MAX_TEMP)
            current_temp = MAX_TEMP;
        if (current_temp < AMBIENT_TEMP)
            current_temp = AMBIENT_TEMP;

        float noise = E2E_LOGGING ? random_float(-NOISE_RANGE, NOISE_RANGE) : 0.0f;
        float simulated_reading = current_temp + noise;

        // Output temperature via DAC
        uint32_t dac_val = (uint32_t)((simulated_reading / MAX_TEMP) * 255.0f);
        if (dac_val > 255)
            dac_val = 255;
        ESP_ERROR_CHECK(dac_oneshot_output_voltage(dac_handle, dac_val));

        ESP_LOGI("SIM", "Temp: %.1fC (noisy: %.1fC) -> DAC: %lu",
                 current_temp, simulated_reading, dac_val);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    init_heater_adc();

    // Configure reset input pin (pull-down so it reads 0 when controller isn't driving it)
    gpio_config_t reset_io = {
        .pin_bit_mask = (1ULL << RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&reset_io);

    ESP_LOGI("SIM", "Bridge Running: Temp DAC on GPIO 25, Heater ADC on GPIO 32, Reset on GPIO %d", RESET_GPIO);
    ESP_LOGI("SIM", "Physics: Ambient=%.1fC, HeatingRate=%.2f, CoolingRate=%.3f, ThermalMass=%.2f",
             AMBIENT_TEMP, HEATING_RATE, COOLING_RATE, THERMAL_MASS);

    physics_simulation_loop();
}