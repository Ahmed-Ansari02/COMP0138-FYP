#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);

int main()
{
    host_log("Fault Test: Infinite Loop (10 iterations then fault)");

    for (int i = 0; i < CONTAINER_FAULT_PRE_ITERATIONS; i++)
    {
        float temp = host_get_temperature();

        if (temp < (CONTAINER_TARGET_TEMP - CONTAINER_FAULT_DEVIATION))
            host_set_heater(1.0f);
        else if (temp > (CONTAINER_TARGET_TEMP + CONTAINER_FAULT_DEVIATION))
            host_set_heater(0.0f);

        host_delay(CONTAINER_FAULT_PERIOD_MS);
    }

    host_log("FAULT: Entering infinite loop NOW");

    // Tight loop with no host function calls — watchdog must detect and terminate
    while (1)
    {
        volatile int x = 0;
        x++;
    }

    return 0;
}
