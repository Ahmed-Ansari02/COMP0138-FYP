// shared_data.h — included by BOTH host and Wasm code
#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#ifdef __wasm__
// Minimal type definitions for WASM (no libc)
typedef unsigned int      uint32_t;
typedef unsigned short    uint16_t;
typedef short             int16_t;
typedef unsigned char     uint8_t;
#else
#include <stdint.h>
#include <stddef.h>
#endif

// Configurable payload size (set via -DPAYLOAD_SIZE=N at compile time)
// Total struct size = 12 + PAYLOAD_SIZE bytes
#ifndef PAYLOAD_SIZE
#define PAYLOAD_SIZE 0
#endif

struct SensorMessage {
    uint32_t timestamp_ms;
    uint16_t sensor_id;
    int16_t  value;
    uint8_t  status;
    uint8_t  priority;
    uint8_t  _pad[2];          // explicit padding to 12 bytes
#if PAYLOAD_SIZE > 0
    uint8_t  payload[PAYLOAD_SIZE];
#endif
};

// Layout checks only on host side (needs <stddef.h> for offsetof)
#ifndef __wasm__
_Static_assert(sizeof(struct SensorMessage) == 12 + PAYLOAD_SIZE,
    "SensorMessage size mismatch");
_Static_assert(offsetof(struct SensorMessage, timestamp_ms) == 0,
    "timestamp_ms offset mismatch");
_Static_assert(offsetof(struct SensorMessage, sensor_id) == 4,
    "sensor_id offset mismatch");
_Static_assert(offsetof(struct SensorMessage, value) == 6,
    "value offset mismatch");
_Static_assert(offsetof(struct SensorMessage, status) == 8,
    "status offset mismatch");
_Static_assert(offsetof(struct SensorMessage, priority) == 9,
    "priority offset mismatch");
#endif

#endif // SHARED_DATA_H
