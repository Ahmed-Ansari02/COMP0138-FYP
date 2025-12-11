#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/dac_oneshot.h" // Requires ESP-IDF v5.x
#include "esp_log.h"
#include "esp_random.h"
#include "driver/mcpwm_cap.h" // Capture Driver

#define CAPTURE_GPIO 27

// --- PINS for TTGO T-Display ---
#define PIN_DAC_CHAN DAC_CHAN_0 // GPIO 25 (Right side, 3rd pin from bottom)
// #define PIN_HEATER_IN 27        // GPIO 27 (Right side, bottom pin)

// --- PHYSICS CONSTANTS (matching sim.py) ---
#define AMBIENT_TEMP 25.0f // Room temp (C)
#define MAX_TEMP 100.0f    // Max allowed temp
#define HEATING_RATE 0.8f  // How fast it gains heat (deg/tick)
#define COOLING_RATE 0.02f // How fast it loses heat to environment
#define THERMAL_MASS 0.95f // Inertia (Higher = Slower/Smoother changes)
#define NOISE_RANGE 0.3f   // Sensor noise +/- range


static float current_temp = 25.0f; // Current temperature state
volatile float received_heater_power = 0.0f;

// Helper function for random float in range
static float random_float(float min, float max)
{
    uint32_t rand_val = esp_random();
    float normalized = (float)rand_val / (float)UINT32_MAX; // 0.0 to 1.0
    return min + normalized * (max - min);
}

static bool on_capture_event(mcpwm_cap_channel_handle_t cap_chan, const mcpwm_capture_event_data_t *edata, void *user_data)
{
    static uint32_t pos_edge_timestamp = 0;
    
    // If Edge is Positive (Low -> High), start timer
    if (edata->cap_edge == MCPWM_CAP_EDGE_POS) {
        pos_edge_timestamp = edata->cap_value;
    } 
    // If Edge is Negative (High -> Low), calculate duration
    else {
        uint32_t neg_edge_timestamp = edata->cap_value;
        uint32_t high_time_us = neg_edge_timestamp - pos_edge_timestamp;
        
        // Calculate Duty Cycle (Assuming 1kHz / 1000us period)
        // 1000us = 100% power
        float power = (float)high_time_us / 1000.0f; 
        
        // Simple filter/clamp
        if(power > 1.0f) power = 1.0f;
        if(power < 0.0f) power = 0.0f;
        
        received_heater_power = power;
    }
    return false;
}

void init_pwm_capture() {
    // 1. Install Capture Timer (1MHz resolution = 1us per tick)
    mcpwm_cap_timer_handle_t cap_timer = NULL;
    mcpwm_capture_timer_config_t cap_conf = {
        .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_capture_timer_new_default(&cap_conf, &cap_timer));

    // 2. Install Capture Channel on GPIO 27
    mcpwm_cap_channel_handle_t cap_chan = NULL;
    mcpwm_capture_channel_config_t chan_conf = {
        .gpio_num = CAPTURE_GPIO,
        .prescale = 1,
    };
    ESP_ERROR_CHECK(mcpwm_capture_channel_new_default(cap_timer, &chan_conf, &cap_chan));

    // 3. Register Callback
    mcpwm_capture_event_callbacks_t cbs = {
        .on_cap = on_capture_event,
    };
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(cap_chan, &cbs, NULL));

    // 4. Start
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_timer_start(cap_timer));
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(cap_chan));
    
    ESP_LOGI("SIM", "PWM Capture Started on Pin %d", CAPTURE_GPIO);
}

void physics_simulation_loop()
{
    // 1. Setup DAC (Output Temperature Voltage)
    dac_oneshot_handle_t dac_handle;
    dac_oneshot_config_t dac_cfg = {.chan_id = PIN_DAC_CHAN};
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac_cfg, &dac_handle));
    while (1)
    {
        int heater_state = gpio_get_level(PIN_HEATER_IN);
        float heater_cmd = (float)heater_state; // 0.0 or 1.0

        // B. Physics Simulation (Newton's Law of Cooling)
        // Energy In: Heater Power
        float energy_in = heater_cmd * HEATING_RATE;

        // Energy Out: Difference between object and room temp
        float energy_out = (current_temp - AMBIENT_TEMP) * COOLING_RATE;

        // Apply Thermal Mass (Smoothing/Lag)
        float target_next_temp = current_temp + energy_in - energy_out;
        current_temp = (current_temp * THERMAL_MASS) + (target_next_temp * (1.0f - THERMAL_MASS));

        // Clamp temperature to valid range
        if (current_temp > MAX_TEMP)
            current_temp = MAX_TEMP;
        if (current_temp < AMBIENT_TEMP)
            current_temp = AMBIENT_TEMP;

        // C. Add sensor noise for realistic readings
        float noise = random_float(-NOISE_RANGE, NOISE_RANGE);
        float simulated_reading = current_temp + noise;

        // D. Output Voltage via DAC
        uint32_t dac_val = (uint32_t)((simulated_reading / MAX_TEMP) * 255.0f);
        if (dac_val > 255)
            dac_val = 255;
        ESP_ERROR_CHECK(dac_oneshot_output_voltage(dac_handle, dac_val));

        ESP_LOGI("SIM", "Temp: %.1fC (noisy: %.1fC) -> DAC: %lu | Heater: %s",
                 current_temp, simulated_reading, dac_val, heater_state ? "ON" : "OFF");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{

    // 2. Setup Digital Input (Read Heater Command)
    init_pwm_capture();
    ESP_LOGI("SIM", "Simulator Running on Pins 25 (DAC) & 27 (Input)");
    ESP_LOGI("SIM", "Physics: Ambient=%.1fC, HeatingRate=%.2f, CoolingRate=%.3f, ThermalMass=%.2f",
             AMBIENT_TEMP, HEATING_RATE, COOLING_RATE, THERMAL_MASS);

    // Start physics simulation loop
    physics_simulation_loop();
}