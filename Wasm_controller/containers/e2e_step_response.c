#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);
extern long long host_get_time_us(void);
extern void host_record_heater_time(long long duration_us);
extern void host_record_temp_get_time(long long duration_us);

int main()
{
    host_log("Starting Step-Response E2E Test...");

    long long t;
    int i, step;

    // Phase 1: Stabilize at ambient (heater OFF)
    host_set_heater(0.0f);
    for (i = 0; i < CONTAINER_E2E_STABILIZE_ITERS; i++)
    {
        t = host_get_time_us();
        host_get_temperature();
        host_record_temp_get_time(host_get_time_us() - t);
        host_delay(CONTAINER_E2E_PERIOD_MS);
    }

    // Phase 2: Repeated step responses
    for (step = 0; step < CONTAINER_E2E_NUM_STEPS; step++)
    {
        // Step ON - this arms E2E detection in reader_task
        t = host_get_time_us();
        host_set_heater(1.0f);
        host_record_heater_time(host_get_time_us() - t);

        // Hold ON, keep collecting metrics
        for (i = 0; i < CONTAINER_E2E_HEAT_ITERS; i++)
        {
            t = host_get_time_us();
            host_get_temperature();
            host_record_temp_get_time(host_get_time_us() - t);
            host_delay(CONTAINER_E2E_PERIOD_MS);
        }

        // Step OFF
        t = host_get_time_us();
        host_set_heater(0.0f);
        host_record_heater_time(host_get_time_us() - t);

        // Cooldown, keep collecting metrics
        for (i = 0; i < CONTAINER_E2E_COOL_ITERS; i++)
        {
            t = host_get_time_us();
            host_get_temperature();
            host_record_temp_get_time(host_get_time_us() - t);
            host_delay(CONTAINER_E2E_PERIOD_MS);
        }
    }

    host_log("Step-Response E2E Test Complete");
    return 0;
}
