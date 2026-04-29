/* IPC Benchmark — Native-call producer (Method 5)
 * Exports produce(i) — called by host each iteration.
 * Writes SensorMessage to linear memory at offset 0.
 */

#include "shared_data.h"
// SensorMessage written at the start of linear memory
static struct SensorMessage *msg = (struct SensorMessage *)0;
static int iteration = 0;

void create_message() {
    msg->timestamp_ms = iteration * 10;
    msg->sensor_id = (unsigned short)(iteration & 0xFFFF);
    msg->value = (short)(iteration % 100);
    msg->status = 1;
    msg->priority = (unsigned char)(iteration & 0x03);
}

struct SensorMessage* produce() {
    create_message();
    iteration = iteration + 1;
    return msg;
}

// Dummy main required by linker
int main() { 
    return 0;
}
