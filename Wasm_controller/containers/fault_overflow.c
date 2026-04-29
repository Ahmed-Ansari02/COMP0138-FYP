#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);

// Recursive function with large stack frame to overflow 2KB WASM stack
void recurse(int depth)
{
    volatile char buf[1024]; // 1KB per frame — overflows in ~2 calls
    buf[0] = (char)depth;
    buf[1023] = (char)depth;
    recurse(depth + 1);
}

int main()
{
    host_log("Fault Test: Stack Overflow (10 iterations then fault)");

    for (int i = 0; i < CONTAINER_FAULT_PRE_ITERATIONS; i++)
    {
        float temp = host_get_temperature();

        if (temp < (CONTAINER_TARGET_TEMP - CONTAINER_FAULT_DEVIATION))
            host_set_heater(1.0f);
        else if (temp > (CONTAINER_TARGET_TEMP + CONTAINER_FAULT_DEVIATION))
            host_set_heater(0.0f);

        host_delay(CONTAINER_FAULT_PERIOD_MS);
    }

    host_log("FAULT: Triggering stack overflow NOW");
    recurse(0);

    return 0;
}
