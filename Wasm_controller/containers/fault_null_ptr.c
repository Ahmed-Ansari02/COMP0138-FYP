#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);

int main()
{
    host_log("Fault Test: OOB Memory Access (10 iterations then fault)");

    for (int i = 0; i < CONTAINER_FAULT_PRE_ITERATIONS; i++)
    {
        float temp = host_get_temperature();

        if (temp < (CONTAINER_TARGET_TEMP - CONTAINER_FAULT_DEVIATION))
            host_set_heater(1.0f);
        else if (temp > (CONTAINER_TARGET_TEMP + CONTAINER_FAULT_DEVIATION))
            host_set_heater(0.0f);

        host_delay(CONTAINER_FAULT_PERIOD_MS);
    }

    host_log("FAULT: Accessing OOB memory NOW");

    // Access address far beyond 64KB linear memory — WAMR traps as "out of bounds memory access"
    volatile int *p = (volatile int *)0xFFFFFF;
    *p = 42;

    return 0;
}
