#include "container_config.h"

extern void host_set_heater(float value);
extern float host_get_temperature(void);
extern void host_delay(int ms);
extern void host_log(const char *msg);

static float clamp(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

int main(void)
{
	host_log("PID Temperature Controller Started");
	host_log("Target: shared config PID regulation");

	const float dt = CONTAINER_PID_PERIOD_MS / 1000.0f;
	float integral = 0.0f;
	float prev_error = 0.0f;
	int log_counter = 0;

	while (1)
	{
		// Measurement
		float temp = host_get_temperature();
		float error = CONTAINER_TARGET_TEMP - temp;

		// Integral with clamp (anti-windup)
		integral += error * dt;
		integral = clamp(integral, CONTAINER_PID_INTEGRAL_MIN, CONTAINER_PID_INTEGRAL_MAX);

		// Derivative on error
		float derivative = (error - prev_error) / dt;

		// PID output -> heater command (duty 0..1)
		float command = (CONTAINER_PID_KP * error) + (CONTAINER_PID_KI * integral) + (CONTAINER_PID_KD * derivative);
		command = clamp(command, CONTAINER_PID_HEATER_MIN, CONTAINER_PID_HEATER_MAX);

		host_set_heater(command);

		// Periodic logging to observe stability
		if (log_counter++ >= CONTAINER_PID_LOG_EVERY)
		{
			log_counter = 0;
			// Keep message short to avoid buffer issues
			host_log("PID loop update");
		}

		prev_error = error;

		host_delay(CONTAINER_PID_PERIOD_MS);
	}

	return 0;
}
