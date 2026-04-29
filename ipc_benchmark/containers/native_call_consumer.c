/* IPC Benchmark — Native-call consumer (Method 5)
 * Exports consume() — called by host each iteration.
 * Reads SensorMessage from linear memory at offset 0 (written by host).
 */

#include "shared_data.h"

// SensorMessage read from the start of linear memory
static struct SensorMessage *msg_in = (struct SensorMessage *)0;

// Returns timestamp_ms to prevent dead-code elimination
void consume(struct SensorMessage *msg) {
    *msg_in = *msg; 
}

// Dummy main required by linker
int main() { return 0; }

