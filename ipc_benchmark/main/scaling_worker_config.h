#ifndef SCALING_WORKER_CONFIG_H
#define SCALING_WORKER_CONFIG_H

#ifdef __wasm__
typedef unsigned int uint32_t;
#else
#include <stdint.h>
#endif

#define SCALING_WORKER_ITERATIONS 120u
#define SCALING_WORKER_DELAY_MS 15u
#define SCALING_WORKER_KERNEL_ITERS 2048u
#define SCALING_WORKER_COOPERATE_CHUNK_ITERS 512u
#define SCALING_WORKER_SEED 0x13579BDFu

static inline uint32_t
scaling_worker_rotl32(uint32_t value, uint32_t shift)
{
    return (uint32_t)((value << shift) | (value >> (32u - shift)));
}

static inline uint32_t
scaling_worker_step_begin(uint32_t acc, uint32_t iteration)
{
    return acc ^ (0x9E3779B9u + (iteration * 0x45D9F3Bu));
}

static inline uint32_t
scaling_worker_kernel_round(uint32_t acc, uint32_t iteration, uint32_t inner_iter)
{
    acc += (0x7F4A7C15u ^ (inner_iter * 17u));
    acc = scaling_worker_rotl32(acc, 5u);
    acc ^= (acc >> 11);
    acc += (iteration + 1u) * (inner_iter + 3u);
    acc = scaling_worker_rotl32((uint32_t)(acc ^ 0xA5A5A5A5u), 7u);
    return acc;
}

static inline uint32_t
scaling_worker_step(uint32_t acc, uint32_t iteration)
{
    acc = scaling_worker_step_begin(acc, iteration);

    for (uint32_t j = 0; j < SCALING_WORKER_KERNEL_ITERS; ++j) {
        acc = scaling_worker_kernel_round(acc, iteration, j);
    }

    return acc;
}

static inline uint32_t
scaling_worker_expected_checksum(void)
{
    uint32_t acc = SCALING_WORKER_SEED;

    for (uint32_t i = 0; i < SCALING_WORKER_ITERATIONS; ++i) {
        acc = scaling_worker_step(acc, i);
    }

    return acc;
}

#endif
