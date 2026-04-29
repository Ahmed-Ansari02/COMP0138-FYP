/* IPC Benchmark — Consumer WASM module (concurrent methods)
 * Calls ipc_recv() to receive a SensorMessage each iteration.
 * Times each recv using host_get_cycles() and reports via host_record_read_time().
 */

#include "shared_data.h"

extern void ipc_recv(struct SensorMessage *buf);
extern unsigned int host_get_cycles(void);
extern void host_record_read_time(unsigned int cycles);

#define NUM_ITERATIONS 10000

int main() {
    struct SensorMessage buf;

    for (unsigned int i = 0; i < NUM_ITERATIONS; i++) {
        unsigned int c0 = host_get_cycles();
        ipc_recv(&buf);
        unsigned int c1 = host_get_cycles();
        host_record_read_time(c1 - c0);

        // Touch the value to prevent optimization
        volatile unsigned int v = buf.timestamp_ms;
        (void)v;
    }
    return 0;
}
