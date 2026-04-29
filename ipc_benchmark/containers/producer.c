/* IPC Benchmark — Producer WASM module (concurrent methods)
 * Fills a SensorMessage struct and calls ipc_send() each iteration.
 * Times each send using host_get_cycles() and reports via host_record_write_time().
 */

#include "shared_data.h"

extern void ipc_send(struct SensorMessage *msg);
extern unsigned int host_get_cycles(void);
extern void host_record_write_time(unsigned int cycles);

#define NUM_ITERATIONS 10000

int main() {
    struct SensorMessage msg;
    msg.status = 1;
    msg.priority = 0;

    for (unsigned int i = 0; i < NUM_ITERATIONS; i++) {
        msg.timestamp_ms = i * 10;
        msg.sensor_id = (unsigned short)(i & 0xFFFF);
        msg.value = (short)(i % 100);
        msg.priority = (unsigned char)(i & 0x03);

        unsigned int c0 = host_get_cycles();
        ipc_send(&msg);
        unsigned int c1 = host_get_cycles();
        host_record_write_time(c1 - c0);
    }
    return 0;
}
