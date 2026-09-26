/* FluxDM — core/include/bandwidth.h
 * Token bucket algorithm for per-download and global bandwidth limiting.
 * Thread-safe. Designed for the epoll event loop: when tokens are exhausted,
 * the caller pauses the segment and schedules a wake-up timer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* ── Token bucket ─────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t    rate_bps;       /* bytes/sec, 0 = unlimited             */
    uint64_t    burst;          /* max bucket size (default = 2× rate)  */
    uint64_t    tokens;         /* current tokens (bytes)               */
    uint64_t    last_refill_ms; /* monotonic timestamp of last refill   */
    pthread_mutex_t lock;
} flux_bucket_t;

/* ── Global limiter (singleton, shared across all downloads) ─────────────── */
typedef struct {
    flux_bucket_t global;
} flux_bw_limiter_t;

/* ── Init / destroy ───────────────────────────────────────────────────────── */
void flux_bw_init(flux_bucket_t *b, uint64_t rate_bps);
void flux_bw_destroy(flux_bucket_t *b);

/* Change the rate at runtime (e.g. from IPC command). */
void flux_bw_set_rate(flux_bucket_t *b, uint64_t rate_bps);

/* ── Consume tokens ───────────────────────────────────────────────────────── */

/* Try to consume `bytes` tokens.
 * Returns 0 if tokens were available (download can proceed).
 * Returns -1 with FLUX_ERR_WOULDBLOCK if throttled.
 * When throttled, *wait_ms is set to how long the caller should sleep
 * before retrying (i.e. before resuming the segment). */
int flux_bw_consume(flux_bucket_t *b, size_t bytes, uint32_t *wait_ms);

/* Non-blocking peek: how many bytes can be sent right now? */
size_t flux_bw_available(flux_bucket_t *b);

/* Manually refill the bucket based on elapsed time (call periodically). */
void flux_bw_refill(flux_bucket_t *b);
