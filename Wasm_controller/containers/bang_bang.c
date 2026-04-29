#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void); // This can stay float
extern void host_delay(int ms);
extern void host_log(const char *msg);

int main()
{
    host_log("Temperature Controller Started");
    host_log("Target: shared config with hysteresis");

    float current_temp;

    // Main control loop
    while (1)
    {
        // Read current temperature
        current_temp = host_get_temperature();

        // Bang-bang control with hysteresis
        // Turn ON heater if temp falls below (target - hysteresis)
        // Turn OFF heater if temp rises above (target + hysteresis)
        if (current_temp < (CONTAINER_TARGET_TEMP - CONTAINER_BANG_BANG_DEVIATION))
        {
            host_set_heater(1.0f);
        }
        else if (current_temp > (CONTAINER_TARGET_TEMP + CONTAINER_BANG_BANG_DEVIATION))
        {
            // Too hot - turn heater OFF
            host_set_heater(0.0f);
        }
        // Within hysteresis band - maintain current state

        // Wait for next control cycle
        host_delay(CONTAINER_BANG_BANG_PERIOD_MS);
    }

    return 0;
}
