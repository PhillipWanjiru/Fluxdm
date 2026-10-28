/* FluxDM — core/src/hostgate.c
 * Per-host connection governor. The policy is described in hostgate.h; this
 * file is the bookkeeping. Nothing here blocks, logs or touches the network:
 * callers pass the time in and act on the verdict, which keeps the whole thing
 * deterministic.
 */

#include "../include/hostgate.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define GATE_TABLE  64u               /* distinct servers tracked at once */

struct flux_hostgate {
    char     host[256];
    uint16_t port;
    uint8_t  used;
    uint8_t  has_refused;             /* the host has said no since it was last forgotten */

    uint32_t active;                  /* connections holding a slot                       */
    uint32_t limit;                   /* connections allowed at once                      */

    uint64_t paused_until_ms;         /* Retry-After: no new connection before this       */
    uint64_t next_start_ms;           /* spacing of new connections while unhappy         */
    uint64_t last_refusal_ms;
    uint64_t last_raise_ms;           /* last time the allowance changed                  */
    uint64_t last_used_ms;            /* for evicting the stalest idle entry              */

    /* Written by the segments' reads without the lock, read under it: __atomic only.   */
    uint64_t served_ms;               /* when the host last fed one of our connections    */

    /* Reporting: what the user has been told, so the log says it once.                 */
    uint64_t lowered_ms;              /* when the allowance was last cut                  */
    uint32_t announced;               /* the allowance the user was last told about       */
    uint8_t  news_due;                /* it was cut since then                            */
    uint8_t  complaint_due;           /* the host started refusing us: not yet reported   */
};

static struct flux_hostgate g_table[GATE_TABLE];
static pthread_mutex_t      g_lock     = PTHREAD_MUTEX_INITIALIZER;
static uint32_t             g_patience = HOSTGATE_PATIENCE_MS;     /* __atomic */

/* ── Table ────────────────────────────────────────────────────────────────── */

/* The entry for host:port. With `create`, a missing entry takes a free row or
 * evicts the stalest idle one; NULL means every row is busy (the caller then
 * runs ungated — losing the policy beats refusing to download). */
static flux_hostgate_t *find_locked(const char *host, uint16_t port, uint64_t now, int create) {
    if (!host) return NULL;

    flux_hostgate_t *free_row = NULL, *stalest = NULL;
    for (unsigned i = 0; i < GATE_TABLE; i++) {
        flux_hostgate_t *g = &g_table[i];
        if (!g->used) {
            if (!free_row) free_row = g;
            continue;
        }
        if (g->port == port && strcasecmp(g->host, host) == 0) return g;
        if (g->active == 0 && (!stalest || g->last_used_ms < stalest->last_used_ms)) stalest = g;
    }
    if (!create) return NULL;

    flux_hostgate_t *g = free_row ? free_row : stalest;
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));
    g->used         = 1;
    g->port         = port;
    g->limit        = HOSTGATE_OPEN;
    g->announced    = HOSTGATE_OPEN;
    g->last_used_ms = now;
    snprintf(g->host, sizeof(g->host), "%s", host);
    return g;
}

/* A host nobody has talked to for a long while gets a clean slate: whatever
 * upset it is probably over, and a stale restriction would only slow us down. */
static void forget_locked(flux_hostgate_t *g, uint64_t now) {
    if (g->has_refused && g->active == 0 && now >= g->last_refusal_ms + HOSTGATE_FORGET_MS) {
        g->limit           = HOSTGATE_OPEN;
        g->has_refused     = 0;
        g->paused_until_ms = 0;
        g->next_start_ms   = 0;
        g->announced       = HOSTGATE_OPEN;      /* a fresh start is a fresh story */
        g->news_due        = 0;
        g->complaint_due   = 0;
    }
}

/* ── Slots ────────────────────────────────────────────────────────────────── */

int flux_gate_acquire(const char *host, uint16_t port, uint64_t now,
                      flux_hostgate_t **gate, uint32_t *wait_ms) {
    *gate    = NULL;
    *wait_ms = 0;

    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 1);
    if (!g) {                                            /* untracked: no gating */
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    g->last_used_ms = now;
    forget_locked(g, now);

    int      granted = 0;
    uint32_t wait    = 0;

    if (now < g->paused_until_ms) {
        wait = (uint32_t)(g->paused_until_ms - now);
    } else {
        int unhappy = g->has_refused && now < g->last_refusal_ms + HOSTGATE_CALM_MS;

        /* Somebody wants a connection the allowance does not cover. If the host
         * has been quiet long enough, try one more: this is how a cut that was
         * too deep (or a one-off hiccup) heals. */
        if (g->active >= g->limit && g->limit < HOSTGATE_OPEN && !unhappy &&
            now >= g->last_raise_ms + HOSTGATE_RAISE_MS) {
            g->limit++;
            g->last_raise_ms = now;
        }

        if (g->active >= g->limit) {
            wait = HOSTGATE_POLL_MS;
        } else if (unhappy && now < g->next_start_ms) {
            wait = (uint32_t)(g->next_start_ms - now);
        } else {
            if (unhappy) g->next_start_ms = now + HOSTGATE_GAP_MS;
            g->active++;
            granted = 1;
            *gate   = g;
        }
    }
    pthread_mutex_unlock(&g_lock);

    *wait_ms = wait;
    return granted;
}

void flux_gate_hold(const char *host, uint16_t port, uint64_t now, flux_hostgate_t **gate) {
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 1);
    if (g) {
        g->active++;
        g->last_used_ms = now;
    }
    pthread_mutex_unlock(&g_lock);
    *gate = g;
}

void flux_gate_release(flux_hostgate_t *g) {
    if (!g) return;
    pthread_mutex_lock(&g_lock);
    if (g->active > 0) g->active--;
    pthread_mutex_unlock(&g_lock);
}

/* ── Feedback ─────────────────────────────────────────────────────────────── */

/* The host refused a connection while `others` other connections were open.
 * That is the evidence of what it tolerates: allow no more than that. */
static uint32_t refuse_locked(flux_hostgate_t *g, uint64_t now, uint32_t retry_after_ms,
                              uint32_t others) {
    uint32_t allowance = others < g->limit ? others : g->limit;
    if (allowance < HOSTGATE_MIN) allowance = HOSTGATE_MIN;
    uint32_t lowered = allowance < g->limit ? allowance : 0u;

    if (!g->has_refused) g->complaint_due = 1;           /* the host has just started refusing us */
    g->limit           = allowance;
    g->has_refused     = 1;
    g->last_refusal_ms = now;
    g->last_raise_ms   = now;                            /* growth restarts from here */
    if (lowered) {
        g->lowered_ms = now;                             /* worth reporting once it stops moving */
        g->news_due   = 1;
    }
    if (retry_after_ms) {
        uint64_t until = now + retry_after_ms;
        if (until > g->paused_until_ms) g->paused_until_ms = until;
    }
    return lowered;
}

uint32_t flux_gate_throttled(flux_hostgate_t *g, uint64_t now, uint32_t retry_after_ms) {
    if (!g) return 0;
    pthread_mutex_lock(&g_lock);
    uint32_t lowered = refuse_locked(g, now, retry_after_ms, g->active > 0 ? g->active - 1u : 0u);
    pthread_mutex_unlock(&g_lock);
    return lowered;
}

uint32_t flux_gate_note_refusal(const char *host, uint16_t port, uint64_t now,
                                uint32_t retry_after_ms) {
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 1);
    uint32_t lowered = g ? refuse_locked(g, now, retry_after_ms, g->active) : 0u;
    pthread_mutex_unlock(&g_lock);
    return lowered;
}

uint32_t flux_gate_pause_ms(const char *host, uint16_t port, uint64_t now) {
    uint32_t pause = 0;
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 0);
    if (g && now < g->paused_until_ms) pause = (uint32_t)(g->paused_until_ms - now);
    pthread_mutex_unlock(&g_lock);
    return pause;
}

/* ── Patience ─────────────────────────────────────────────────────────────── */

void flux_gate_served(flux_hostgate_t *g, uint64_t now) {
    /* The caller holds a slot, so the row cannot be recycled under us. A racing
     * thread may store a time a few milliseconds older: harmless. */
    if (g) __atomic_store_n(&g->served_ms, now, __ATOMIC_RELAXED);
}

uint64_t flux_gate_waited_ms(const char *host, uint16_t port, uint64_t refused_since,
                             uint64_t now) {
    uint64_t since = refused_since;
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 0);
    if (g) {
        uint64_t served = __atomic_load_n(&g->served_ms, __ATOMIC_RELAXED);
        if (served > since) since = served;
    }
    pthread_mutex_unlock(&g_lock);
    return now > since ? now - since : 0u;               /* a stamp from a later clock reading: no wait */
}

/* ── News ─────────────────────────────────────────────────────────────────── */

flux_gate_news_t flux_gate_news(const char *host, uint16_t port, uint64_t now) {
    flux_gate_news_t news = { 0, 0 };
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, now, 0);
    if (g) {
        news.complaint = g->complaint_due;
        g->complaint_due = 0;
        if (g->news_due && now >= g->lowered_ms + HOSTGATE_SETTLE_MS) {
            g->news_due = 0;
            if (g->limit != g->announced) {              /* 3 -> 2 -> 3 -> 2 is one piece of news */
                g->announced = g->limit;
                news.settled = g->limit;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return news;
}

/* ── Policy helpers ───────────────────────────────────────────────────────── */

uint32_t flux_gate_backoff_ms(unsigned n, uint32_t salt) {
    uint32_t ms = 1000u << (n > 5u ? 5u : n);
    if (ms > 30000u) ms = 30000u;
    uint32_t h = salt * 2654435761u + (uint32_t)n * 40503u;
    int32_t  j = (int32_t)(h % 25u) - 12;                /* -12 .. +12 percent */
    return (uint32_t)((int64_t)ms + (int64_t)ms * j / 100);
}

uint32_t flux_gate_patience_ms(void) {
    return __atomic_load_n(&g_patience, __ATOMIC_RELAXED);
}

void flux_gate_set_patience_ms(uint32_t ms) {
    __atomic_store_n(&g_patience, ms, __ATOMIC_RELAXED);
}

/* ── Introspection ────────────────────────────────────────────────────────── */

int flux_gate_stats(const char *host, uint16_t port, uint32_t *limit, uint32_t *active) {
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    flux_hostgate_t *g = find_locked(host, port, 0, 0);
    if (g) {
        if (limit)  *limit  = g->limit;
        if (active) *active = g->active;
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void flux_gate_reset(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_table, 0, sizeof(g_table));
    pthread_mutex_unlock(&g_lock);
}
