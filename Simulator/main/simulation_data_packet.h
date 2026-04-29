#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t device_id;   // 1 = Controller
    uint8_t id;    // actuator: 1 || sensor: 0
    float   value;       // The data we are sending
    uint32_t counter;    // To see if packets are dropped
} SimPacket;