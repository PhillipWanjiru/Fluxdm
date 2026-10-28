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
    uint64_t    burst;          /* max bucket size = rate × burst_ms     */
    uint32_t    burst_ms;       /* bucket depth in milliseconds of rate
                                 * (default 2000, see flux_bw_set_burst_ms) */
    uint64_t    tokens;         /* current tokens (bytes)               */
    uint64_t    last_refill_ms; /* monotonic timestamp of last refill   */
    pthread_mutex_t lock;
    uint64_t    debt;           /* bytes taken beyond the available tokens
                                 * (see flux_bw_charge); repaid from refill */
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

/* How deep the bucket is, in milliseconds of the configured rate. A deep
 * bucket lets a transfer start fast and then settle at the rate; a shallow one
 * holds it to the rate from the first byte. The default is 2000 ms; downloads
 * use 250 ms so that a 3 MB file under a 1 MB/s limit takes 3 s, not 1 s. */
void flux_bw_set_burst_ms(flux_bucket_t *b, uint32_t burst_ms);

/* ── Consume tokens ───────────────────────────────────────────────────────── */

/* Try to consume `bytes` tokens.
 * Returns 0 if tokens were available (download can proceed).
 * Returns -1 with FLUX_ERR_WOULDBLOCK if throttled.
 * When throttled, *wait_ms is set to how long the caller should sleep
 * before retrying (i.e. before resuming the segment). */
int flux_bw_consume(flux_bucket_t *b, size_t bytes, uint32_t *wait_ms);

/* Charge `bytes` that have ALREADY been received. Unlike flux_bw_consume()
 * this never refuses: if the bucket is short, the shortfall becomes debt
 * that later refills repay first. Returns how many milliseconds the caller
 * should stop reading before reading again (0 = keep going).
 *
 * This is what the segment layer uses: by the time we know how many bytes a
 * socket read produced they are already in our buffer, so the honest thing
 * is to charge them and pause the *next* read, rather than refuse the
 * charge and lose track of bytes that were really transferred. Averaged
 * over time the transfer rate converges on rate_bps. */
uint32_t flux_bw_charge(flux_bucket_t *b, size_t bytes);

/* Current configured rate in bytes/sec (0 = unlimited). Thread-safe. */
uint64_t flux_bw_rate(flux_bucket_t *b);

/* Non-blocking peek: how many bytes can be sent right now? */
size_t flux_bw_available(flux_bucket_t *b);

/* Manually refill the bucket based on elapsed time (call periodically). */
void flux_bw_refill(flux_bucket_t *b);
