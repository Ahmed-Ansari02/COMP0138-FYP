#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void); 
extern void host_delay(int ms);
extern void host_log(const char *msg);
extern long long host_get_time_us(void);
extern void host_record_heater_time(long long duration_us);
extern void host_record_temp_get_time(long long duration_us);

int counter = CONTAINER_TOGGLE_ITERATIONS;

int main()
{
    float current_temp;

    // Main control loop
    while (counter > 0)
    {
        // Measure get_temperature
        long long t_start = host_get_time_us();
        current_temp = host_get_temperature();
        long long t_end = host_get_time_us();
        host_record_temp_get_time(t_end - t_start);

        // Measure set_heater (ON)
        t_start = host_get_time_us();
        host_set_heater(1.0f);
        t_end = host_get_time_us();
        host_record_heater_time(t_end - t_start);
        
        host_delay(CONTAINER_TOGGLE_ON_PERIOD_MS);
        
        // Measure set_heater (OFF)
        t_start = host_get_time_us();
        host_set_heater(0.0f);
        t_end = host_get_time_us();
        host_record_heater_time(t_end - t_start);

        host_delay(CONTAINER_TOGGLE_PERIOD_MS);
        counter --;
    }

    return 0;
}
