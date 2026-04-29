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
    host_log("Bang-Bang Finite Controller Started");

    for (int i = 0; i < CONTAINER_ITERATIONS; i++)
    {
        long long t_start = host_get_time_us();
        float current_temp = host_get_temperature();
        host_record_temp_get_time(host_get_time_us() - t_start);

        t_start = host_get_time_us();
        if (current_temp < (CONTAINER_TARGET_TEMP - CONTAINER_BANG_BANG_DEVIATION))
        {
            host_set_heater(1.0f);
        }
        else if (current_temp > (CONTAINER_TARGET_TEMP + CONTAINER_BANG_BANG_DEVIATION))
        {
            host_set_heater(0.0f);
        }
        host_record_heater_time(host_get_time_us() - t_start);

        host_delay(CONTAINER_BANG_BANG_FINITE_PERIOD_MS);
    }

    host_log("Bang-Bang Finite Controller Complete");
    return 0;
}
