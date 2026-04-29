#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);
extern long long host_get_time_us(void);
extern void host_record_heater_time(long long duration_us);
extern void host_record_temp_get_time(long long duration_us);

static float clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int main(void)
{
    host_log("PID Finite Controller Started");

    const float dt = CONTAINER_PID_FINITE_PERIOD_MS / 1000.0f;
    float integral = 0.0f;
    float prev_error = 0.0f;

    for (int i = 0; i < CONTAINER_ITERATIONS; i++)
    {
        long long t_start = host_get_time_us();
        float temp = host_get_temperature();
        host_record_temp_get_time(host_get_time_us() - t_start);
        float error = CONTAINER_TARGET_TEMP - temp;

        integral += error * dt;
        integral = clamp(integral, CONTAINER_PID_INTEGRAL_MIN, CONTAINER_PID_INTEGRAL_MAX);

        float derivative = (error - prev_error) / dt;

        float command = (CONTAINER_PID_KP * error) + (CONTAINER_PID_KI * integral) + (CONTAINER_PID_KD * derivative);
        command = clamp(command, CONTAINER_PID_HEATER_MIN, CONTAINER_PID_HEATER_MAX);

        t_start = host_get_time_us();
        host_set_heater(command);
        host_record_heater_time(host_get_time_us() - t_start);

        prev_error = error;
        host_delay(CONTAINER_PID_FINITE_PERIOD_MS);
    }

    host_log("PID Finite Controller Complete");
    return 0;
}
