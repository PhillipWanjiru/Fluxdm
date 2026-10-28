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

    /* Less than one byte's worth of time has passed (low rates, very
     * frequent calls): leave last_refill_ms alone so the fraction keeps
     * accumulating instead of being thrown away on every call, which would
     * make a slow limit run measurably slower than configured. */
    if (generated == 0) return;

    /* Repay outstanding debt before any tokens accumulate. */
    if (b->debt > 0) {
        uint64_t pay = generated < b->debt ? generated : b->debt;
        b->debt   -= pay;
        generated -= pay;
    }

    b->tokens = b->tokens + generated;
    if (b->tokens > b->burst) b->tokens = b->burst;
    b->last_refill_ms = now_ms;
}

/* The bucket must hold at least one smallest read, or a very low limit would
 * never accumulate enough to pass anything. */
#define BW_MIN_BURST       4096u
#define BW_DEFAULT_BURST_MS 2000u

static uint64_t burst_for(uint64_t rate_bps, uint32_t burst_ms) {
    if (rate_bps == 0) return UINT64_MAX;
    uint64_t burst = (rate_bps / 1000u) * burst_ms + ((rate_bps % 1000u) * burst_ms) / 1000u;
    return burst < BW_MIN_BURST ? BW_MIN_BURST : burst;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void flux_bw_init(flux_bucket_t *b, uint64_t rate_bps) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    b->rate_bps       = rate_bps;
    b->burst_ms       = BW_DEFAULT_BURST_MS;
    b->burst          = burst_for(rate_bps, b->burst_ms);
    b->tokens         = b->burst;
    b->last_refill_ms = bw_now_ms();
}

void flux_bw_set_burst_ms(flux_bucket_t *b, uint32_t burst_ms) {
    pthread_mutex_lock(&b->lock);
    b->burst_ms = burst_ms;
    b->burst    = burst_for(b->rate_bps, burst_ms);
    if (b->tokens > b->burst) b->tokens = b->burst;
    pthread_mutex_unlock(&b->lock);
}

void flux_bw_destroy(flux_bucket_t *b) {
    pthread_mutex_destroy(&b->lock);
}

void flux_bw_set_rate(flux_bucket_t *b, uint64_t rate_bps) {
    pthread_mutex_lock(&b->lock);
    b->rate_bps = rate_bps;
    b->burst    = burst_for(rate_bps, b->burst_ms);
    if (b->tokens > b->burst) b->tokens = b->burst;
    if (rate_bps == 0) b->debt = 0;          /* unlimited: nothing to repay */
    b->last_refill_ms = bw_now_ms();         /* don't credit time spent at the old rate */
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

uint32_t flux_bw_charge(flux_bucket_t *b, size_t bytes) {
    pthread_mutex_lock(&b->lock);

    if (b->rate_bps == 0) {                  /* unlimited */
        pthread_mutex_unlock(&b->lock);
        return 0;
    }

    _refill_locked(b, bw_now_ms());

    if (b->tokens >= (uint64_t)bytes) {
        b->tokens -= (uint64_t)bytes;
    } else {
        b->debt  += (uint64_t)bytes - b->tokens;
        b->tokens = 0;
    }

    uint32_t wait_ms = 0;
    if (b->debt > 0) {
        uint64_t ms = (b->debt * 1000u) / b->rate_bps + 1u;
        wait_ms = (uint32_t)(ms > 60000u ? 60000u : ms);
    }
    pthread_mutex_unlock(&b->lock);
    return wait_ms;
}

uint64_t flux_bw_rate(flux_bucket_t *b) {
    pthread_mutex_lock(&b->lock);
    uint64_t r = b->rate_bps;
    pthread_mutex_unlock(&b->lock);
    return r;
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
