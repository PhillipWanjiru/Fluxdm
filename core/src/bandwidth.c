/* FluxDM — core/src/bandwidth.c */

#include "../include/bandwidth.h"
#include "../include/errors.h"
#include <string.h>
#include <time.h>

/* ── Monotonic clock ──────────────────────────────────────────────────────── */
static uint64_t bw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

/* ── Internal refill (must be called with lock held) ─────────────────────── */
static void _refill_locked(flux_bucket_t *b, uint64_t now_ms) {
    if (b->rate_bps == 0) return;   /* unlimited */

    uint64_t elapsed = now_ms - b->last_refill_ms;
    if (elapsed == 0) return;

    /* tokens generated = rate * elapsed_seconds */
    uint64_t generated = (b->rate_bps * elapsed) / 1000u;
    b->tokens = b->tokens + generated;
    if (b->tokens > b->burst) b->tokens = b->burst;
    b->last_refill_ms = now_ms;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void flux_bw_init(flux_bucket_t *b, uint64_t rate_bps) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    b->rate_bps       = rate_bps;
    b->burst          = rate_bps > 0 ? rate_bps * 2 : UINT64_MAX;
    b->tokens         = b->burst;
    b->last_refill_ms = bw_now_ms();
}

void flux_bw_destroy(flux_bucket_t *b) {
    pthread_mutex_destroy(&b->lock);
}

void flux_bw_set_rate(flux_bucket_t *b, uint64_t rate_bps) {
    pthread_mutex_lock(&b->lock);
    b->rate_bps = rate_bps;
    b->burst    = rate_bps > 0 ? rate_bps * 2 : UINT64_MAX;
    if (b->tokens > b->burst) b->tokens = b->burst;
    pthread_mutex_unlock(&b->lock);
}

int flux_bw_consume(flux_bucket_t *b, size_t bytes, uint32_t *wait_ms) {
    if (wait_ms) *wait_ms = 0;

    pthread_mutex_lock(&b->lock);

    /* Unlimited: always allow */
    if (b->rate_bps == 0) {
        pthread_mutex_unlock(&b->lock);
        return 0;
    }

    uint64_t now = bw_now_ms();
    _refill_locked(b, now);

    if (b->tokens >= (uint64_t)bytes) {
        b->tokens -= (uint64_t)bytes;
        pthread_mutex_unlock(&b->lock);
        return 0;
    }

    /* Insufficient tokens — calculate how long to wait */
    uint64_t deficit = (uint64_t)bytes - b->tokens;
    uint32_t ms_needed = (uint32_t)((deficit * 1000u) / b->rate_bps) + 1u;
    if (wait_ms) *wait_ms = ms_needed;

    pthread_mutex_unlock(&b->lock);

    flux_set_error(FLUX_ERR_WOULDBLOCK, "bandwidth throttled, retry in %u ms", ms_needed);
    return -1;
}

size_t flux_bw_available(flux_bucket_t *b) {
    pthread_mutex_lock(&b->lock);
    if (b->rate_bps == 0) {
        pthread_mutex_unlock(&b->lock);
        return SIZE_MAX;
    }
    _refill_locked(b, bw_now_ms());
    size_t avail = (size_t)(b->tokens < SIZE_MAX ? b->tokens : SIZE_MAX);
    pthread_mutex_unlock(&b->lock);
    return avail;
}

void flux_bw_refill(flux_bucket_t *b) {
    pthread_mutex_lock(&b->lock);
    _refill_locked(b, bw_now_ms());
    pthread_mutex_unlock(&b->lock);
}
