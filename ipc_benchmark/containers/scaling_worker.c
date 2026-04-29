/* Low-memory worker used by the WAMR scaling benchmark.
 * It performs a fixed integer-only compute kernel and yields through a host
 * delay each iteration so the host can sample steady-state CPU usage.
 */

#include "scaling_worker_config.h"

extern void host_worker_started(void);
extern void host_report_checksum(unsigned int checksum);
extern void host_delay(unsigned int delay_ms);
extern void host_cooperate(void);

static volatile unsigned int g_sink = 0;

int
main(void)
{
    uint32_t acc = SCALING_WORKER_SEED;

    host_worker_started();

    for (uint32_t i = 0; i < SCALING_WORKER_ITERATIONS; ++i) {
        acc = scaling_worker_step_begin(acc, i);

        for (uint32_t j = 0; j < SCALING_WORKER_KERNEL_ITERS; ++j) {
            acc = scaling_worker_kernel_round(acc, i, j);

            if (((j + 1u) % SCALING_WORKER_COOPERATE_CHUNK_ITERS) == 0u
                && (j + 1u) < SCALING_WORKER_KERNEL_ITERS) {
                host_cooperate();
            }
        }

        g_sink ^= acc;
        host_delay(SCALING_WORKER_DELAY_MS);
    }

    g_sink ^= acc;
    host_report_checksum(acc);

    return 0;
}
