/* FluxDM — core/include/hostgate.h
 * Per-host connection governor: how many connections a server will put up with.
 *
 * Every segment is a TCP connection. Open too many to one server and it starts
 * saying no — HTTP 429 (Too Many Requests) or 503. If every segment retried on
 * its own clock the refusals would simply continue, and the download would die
 * after its last retry. The gate is the one place that knows how many
 * connections are open to a host and how many that host has shown it accepts:
 *
 *   - A host that has not complained may have up to HOSTGATE_OPEN connections.
 *   - A refusal (flux_gate_throttled) lowers the allowance to the number of
 *     other connections that coexist with it — the server has just told us
 *     that is what it accepts — and a Retry-After pauses the host that long.
 *   - Segments that are refused do not retry blindly: they wait for a free
 *     slot (flux_gate_acquire tells them when to ask again), and new
 *     connections are spaced out while the host is unhappy.
 *   - After a calm stretch the allowance creeps back up, one connection at a
 *     time, and only while somebody is actually waiting for it.
 *   - Patience (how long a refused download keeps asking) is measured against
 *     the host's silence, not against the queue: while the host is feeding any of
 *     our connections it is working, and waiting for a slot costs nothing
 *     (flux_gate_served, flux_gate_waited_ms).
 *
 * Additive increase, multiplicative-ish decrease: the shape TCP uses to find
 * the capacity of a link, applied to a server's patience.
 *
 * All state is process-wide (downloads to the same host share one entry) and
 * guarded by one mutex — every download runs on its own worker thread. Time is
 * passed in, so the policy is deterministic and unit-testable.
 */
#pragma once

#include <stdint.h>

/* ── Tunables ─────────────────────────────────────────────────────────────── */
#define HOSTGATE_OPEN           128u      /* allowance of a host that has not complained   */
#define HOSTGATE_MIN            1u        /* never starve a host completely                */
#define HOSTGATE_POLL_MS        100u      /* how long a segment without a slot sleeps       */
#define HOSTGATE_CALM_MS        20000u    /* quiet time before the allowance may grow       */
#define HOSTGATE_RAISE_MS       5000u     /* ...and then it grows by one per this interval  */
#define HOSTGATE_GAP_MS         150u      /* spacing of new connections while unhappy       */
#define HOSTGATE_FORGET_MS      300000u   /* an idle host's complaints are forgotten after this */
#define HOSTGATE_SETTLE_MS      2000u     /* quiet time after a cut before it is worth reporting */

/* How long a download keeps waiting for a server that answers 429 / 503 before it
 * gives up (milliseconds). The clock starts at the first refusal of an unbroken
 * run and restarts whenever the host serves one of our connections: see
 * flux_gate_waited_ms(). */
#define HOSTGATE_PATIENCE_MS    300000u

/* Opaque entry; the pointer stays valid for as long as the caller holds a slot. */
typedef struct flux_hostgate flux_hostgate_t;

/* ── Slots ────────────────────────────────────────────────────────────────── */

/* Ask for a slot for a new connection to host:port.
 * Returns 1 when granted: *gate must be passed to flux_gate_release() when the
 * connection closes (it is NULL when the host is not tracked — the table is
 * full of busy hosts — and releasing NULL is a no-op).
 * Returns 0 when the host cannot take another connection now: *wait_ms (always
 * >= 1) says how long to wait before asking again. */
int  flux_gate_acquire(const char *host, uint16_t port, uint64_t now_ms,
                       flux_hostgate_t **gate, uint32_t *wait_ms);

/* Count a connection that is already open (the probe's, adopted by a segment)
 * without asking: it holds a slot even if the host is over its allowance. */
void flux_gate_hold(const char *host, uint16_t port, uint64_t now_ms, flux_hostgate_t **gate);

/* Give a slot back. NULL is ignored. */
void flux_gate_release(flux_hostgate_t *gate);

/* ── Feedback ─────────────────────────────────────────────────────────────── */

/* The server refused the connection that holds `gate` (HTTP 429 / 503).
 * `retry_after_ms` is its Retry-After (0 = none). Call before releasing the slot.
 * Returns the new allowance when this lowered it, 0 otherwise (tests, diagnostics:
 * the user hears about it through flux_gate_news()). */
uint32_t flux_gate_throttled(flux_hostgate_t *gate, uint64_t now_ms, uint32_t retry_after_ms);

/* The same for a request that holds no slot (the probe). */
uint32_t flux_gate_note_refusal(const char *host, uint16_t port, uint64_t now_ms,
                                uint32_t retry_after_ms);

/* Milliseconds until a Retry-After pause on host:port ends (0 = not paused). */
uint32_t flux_gate_pause_ms(const char *host, uint16_t port, uint64_t now_ms);

/* ── Patience ─────────────────────────────────────────────────────────────── */

/* The host just fed us: a connection that holds `gate` received body bytes at
 * `now_ms`. Lock-free — cheap enough for every read. NULL is ignored (an
 * untracked host). */
void flux_gate_served(flux_hostgate_t *gate, uint64_t now_ms);

/* How long we have been waiting on a refusing host, in milliseconds: the time since
 * the current run of refusals began (`refused_since_ms`) or since the host last
 * served any of our connections, whichever is later.
 *
 * A server that is feeding our other connections is not ignoring us — a segment
 * queued behind its connection limit is simply taking its turn, and must not fail
 * with "the server kept refusing" because its first refusal was a long time ago.
 * Only silence counts: a host that serves nobody lets the clock run. */
uint64_t flux_gate_waited_ms(const char *host, uint16_t port, uint64_t refused_since_ms,
                             uint64_t now_ms);

/* ── News ─────────────────────────────────────────────────────────────────── */

/* What is worth telling the user about a host — each item once, however many
 * segments saw it: a wave of 24 refusals is two lines of log, not 24. */
typedef struct {
    uint8_t  complaint;   /* the host has started refusing us: its first refusal since it was forgotten */
    uint32_t settled;     /* its allowance has come to rest at this many connections (0 = nothing new)  */
} flux_gate_news_t;

/* Collect the news for host:port. `settled` is reported once the allowance has
 * stopped dropping for HOSTGATE_SETTLE_MS, and only when it differs from what the
 * user was last told — the allowance dipping 3 -> 2 again every time the host
 * rejects a probe for more is not news. Cheap enough to ask whenever a segment
 * looks for a slot. */
flux_gate_news_t flux_gate_news(const char *host, uint16_t port, uint64_t now_ms);

/* ── Policy helpers ───────────────────────────────────────────────────────── */

/* Delay before retry number `n` (0-based) after a refusal: 1 s, 2 s, 4 s ...
 * capped at 30 s, with +-12% jitter derived from `salt` so that sibling
 * segments do not come back in lockstep. */
uint32_t flux_gate_backoff_ms(unsigned n, uint32_t salt);

/* How long to keep waiting for a refusing server (see HOSTGATE_PATIENCE_MS). */
uint32_t flux_gate_patience_ms(void);
void     flux_gate_set_patience_ms(uint32_t ms);

/* ── Introspection (tests, diagnostics) ───────────────────────────────────── */

/* Current allowance and open connections of a tracked host; -1 if untracked. */
int  flux_gate_stats(const char *host, uint16_t port, uint32_t *limit, uint32_t *active);

/* Forget every host. Only for tests: no connection may hold a slot. */
void flux_gate_reset(void);
