

extern void host_set_heater(int value);
extern float host_get_temperature(void); // This can stay float
extern void host_delay(int ms);
extern void host_log(const char *msg);

// Control parameters
#define TARGET_TEMP     50.0f   // Target temperature in Celsius
#define HYSTERESIS      1.0f    // +/- 1°C hysteresis band
#define CONTROL_PERIOD  100     // Control loop period in ms

int main()
{
    host_log("Temperature Controller Started");
    host_log("Target: 50C with +/-1C hysteresis");

    float current_temp;
    int heater_state = 0;

    // Main control loop
    while (1)
    {
        // Read current temperature
        current_temp = host_get_temperature();

        // Bang-bang control with hysteresis
        // Turn ON heater if temp falls below (target - hysteresis)
        // Turn OFF heater if temp rises above (target + hysteresis)
        if (current_temp < (TARGET_TEMP - HYSTERESIS))
        {
            // Too cold - turn heater ON
            if (heater_state == 0)
            {
                host_set_heater(1);
                heater_state = 1;
                host_log("Heater ON - temp below threshold");
            }
        }
        else if (current_temp > (TARGET_TEMP + HYSTERESIS))
        {
            // Too hot - turn heater OFF
            if (heater_state == 1)
            {
                host_set_heater(0);
                heater_state = 0;
                host_log("Heater OFF - temp above threshold");
            }
        }
        // Within hysteresis band - maintain current state

        // Wait for next control cycle
        host_delay(CONTROL_PERIOD);
    }

    return 0;
}
