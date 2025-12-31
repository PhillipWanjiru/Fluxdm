/* FluxDM — core/include/util.h
 * Tiny shared helpers. Header-only so every translation unit (C and the
 * Phase 2 C++ engine) can use them without another link dependency.
 */
#pragma once

#include <stdint.h>
#include <time.h>

/* Monotonic milliseconds. Never goes backwards, unaffected by wall-clock
 * changes (NTP, DST, manual date edits), which is what every timeout and
 * rate calculation in the core needs. */
static inline uint64_t flux_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Smaller / larger of two unsigned 64-bit values. */
static inline uint64_t flux_min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t flux_max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }
