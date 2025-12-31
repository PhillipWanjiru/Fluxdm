/* FluxDM — core/src/downloader.c
 *
 * One download = one worker thread = one epoll loop. The worker's life:
 *
 *   probe ─▶ prepare ─▶ run ─▶ complete
 *     │         │         │        │
 *     └─────────┴─────────┴────────┴──▶ pause / stop / fail   (each cleans up
 *                                                              after itself)
 *
 *   probe    One ranged GET (bytes=0-0) discovers size, range support and the
 *            validators, following redirects and retrying transient failures.
 *            It runs on plain poll() loops that also watch the wake-up eventfd,
 *            so a pause or cancel interrupts it at once (DNS is the only
 *            blocking call). Not HEAD: some servers drop HEAD requests.
 *   prepare  Decide whether the partial file on disk can be resumed (same URL
 *            or same validators, same size, contiguous coverage); open and
 *            pre-allocate the output; lay out the segments.
 *   run      Drive the segments from one epoll loop; emit progress; checkpoint
 *            the resume state.
 *   finish   Exactly one of: complete (verify, delete resume state), pause
 *            (flush + checkpoint), stop (keep or delete), fail (flush +
 *            checkpoint, report).
 *
 * The IPC thread talks to the worker through atomics and an eventfd and is the
 * sole owner of the registry: it joins and frees finished downloads, so a
 * worker never frees itself and an id lookup is always safe to dereference.
 */

#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/hostgate.h"
#include "../include/util.h"
#include "../include/protocol/http.h"
#include "../include/protocol/https.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Tunables ─────────────────────────────────────────────────────────────── */
#define PROBE_ATTEMPTS      4            /* tries for a transiently failing probe   */
#define PROBE_BACKOFF_MS    500u         /* first wait between probe attempts       */
#define PROBE_RETRY_AFTER_MAX_MS 10000u  /* never obey a longer Retry-After here    */
#define PROBE_FIRST_ADDR_MS 4000u        /* connect budget for all but the last address */
#define MAX_ADDRS           8
#define PAUSE_WAIT_MS       2000u        /* how long pause/resume wait for the worker */
#define STOP_WAIT_MS        3000u
#define HASH_BLOCK          (1u << 20)
#define WORKER_STACK        (512u * 1024u)
#define DEFAULT_SEGMENTS    8
#define ALIGN_SEG           4096u        /* segment boundaries fall on page boundaries */
#define DL_BW_BURST_MS      250u         /* limiter depth: hold a transfer to its limit from the start */

_Static_assert(FLUX_REG_BUCKETS == 1024, "reg_slot() assumes 1024 buckets");

/* Cross-thread fields are always touched through these. */
#define ALOAD(p)       __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ASTORE(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)

/* What the run loop / completion step tell dl_run() to do next. */
typedef enum {
    LOOP_DONE     = 0,   /* every segment is complete                          */
    LOOP_PAUSE    = 1,   /* a pause request was claimed                        */
    LOOP_STOP     = 2,   /* stop_mode is set                                   */
    LOOP_FINISHED = 3,   /* final state already set and its event emitted      */
} loop_result_t;

/* ═══ Small helpers ═══════════════════════════════════════════════════════ */

static dl_state_t get_state(const flux_download_t *dl) { return (dl_state_t)ALOAD(&dl->state); }
static void       set_state(flux_download_t *dl, dl_state_t s) { ASTORE(&dl->state, (uint32_t)s); }

static void emit(flux_download_t *dl, flux_msg_type_t ev) {
    if (dl->on_event) dl->on_event(dl, ev);
}

static int dl_alive(const flux_download_t *dl) {
    return dl->thread_started && !ALOAD(&dl->finished);
}

static const char *state_name(dl_state_t s) {
    switch (s) {
    case DL_IDLE:      return "idle";
    case DL_PROBING:   return "connecting";
    case DL_RUNNING:   return "running";
    case DL_PAUSED:    return "paused";
    case DL_COMPLETE:  return "complete";
    case DL_ERROR:     return "failed";
    case DL_CANCELLED: return "cancelled";
    }
    return "unknown";
}

/* The URL for log lines: no credentials, no query string (signed URLs carry
 * their secrets there). */
static const char *log_url(const flux_download_t *dl) {
    static _Thread_local char buf[320];
    const http_url_t *u = &dl->origin;
    size_t plen = strcspn(u->path, "?");
    if (plen > 200) plen = 200;
    snprintf(buf, sizeof(buf), "%s://%s%.*s", u->scheme, u->host, (int)plen, u->path);
    return buf;
}

/* A pause or stop request is pending. */
static int interrupted(const flux_download_t *dl) {
    return ALOAD(&dl->stop_mode) != DL_STOP_NONE || ALOAD(&dl->want_paused) != 0;
}

/* The worker takes ownership of a pause request (1 → 2). Fails if a resume
 * withdrew it first. */
static int claim_pause(flux_download_t *dl) {
    int expected = 1;
    return __atomic_compare_exchange_n(&dl->want_paused, &expected, 2, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Poke the worker out of epoll_wait()/poll(). Safe from any thread. */
static void wake_worker(flux_download_t *dl) {
    pthread_mutex_lock(&dl->lock);
    if (dl->wake_fd >= 0) {
        uint64_t one = 1;
        ssize_t  w   = write(dl->wake_fd, &one, sizeof(one));
        (void)w;                                  /* EAGAIN = already signalled */
    }
    pthread_mutex_unlock(&dl->lock);
}

static void drain_wake(const flux_download_t *dl) {
    uint64_t v;
    ssize_t  r = read(dl->wake_fd, &v, sizeof(v));
    (void)r;
}

/* Wait until the worker has exited, at most `timeout_ms`. 1 = exited. */
static int wait_finished(flux_download_t *dl, unsigned timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec  += (time_t)(timeout_ms / 1000u);
    ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&dl->lock);
    while (!ALOAD(&dl->finished)) {
        if (pthread_cond_timedwait(&dl->cv, &dl->lock, &ts) == ETIMEDOUT) break;
    }
    int done = ALOAD(&dl->finished);
    pthread_mutex_unlock(&dl->lock);
    return done;
}

/* Number of bytes that are really on disk. Worker only. */
static uint64_t bytes_on_disk(const flux_download_t *dl) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < dl->num_segs; i++)
        if (dl->segs[i]) sum += dl->segs[i]->written;
    return sum;
}

static void remove_resume_files(const flux_download_t *dl) {
    char tmp[sizeof(dl->part_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dl->part_path);
    unlink(dl->part_path);
    unlink(tmp);
}

/* ═══ Allocation ══════════════════════════════════════════════════════════ */

static flux_download_t *dl_alloc(void) {
    flux_download_t *dl = calloc(1, sizeof(*dl));
    if (!dl) return NULL;

    /* calloc gives 0, and 0 is a perfectly valid fd (stdin): every descriptor
     * is "not open" only when it is -1. */
    dl->file_fd  = -1;
    dl->epoll_fd = -1;
    dl->wake_fd  = -1;

    pthread_mutex_init(&dl->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   /* immune to wall-clock jumps */
    pthread_cond_init(&dl->cv, &ca);
    pthread_condattr_destroy(&ca);
    flux_bw_init(&dl->bw_bucket, 0);
    flux_bw_set_burst_ms(&dl->bw_bucket, DL_BW_BURST_MS);
    return dl;
}

/* Release everything. The worker must be gone (joined or never started). */
static void dl_free(flux_download_t *dl) {
    if (!dl) return;
    for (unsigned i = 0; i < FLUX_DL_MAX_SEGMENTS; i++) flux_seg_free(dl->segs[i]);
    if (dl->probe_conn) flux_conn_close(dl->probe_conn);
    free(dl->probe_resp);
    free(dl->resume);
    if (dl->file_fd  >= 0) close(dl->file_fd);
    if (dl->epoll_fd >= 0) close(dl->epoll_fd);
    if (dl->wake_fd  >= 0) close(dl->wake_fd);
    flux_bw_destroy(&dl->bw_bucket);
    pthread_cond_destroy(&dl->cv);
    pthread_mutex_destroy(&dl->lock);
    free(dl);
}

/* ═══ Registry ════════════════════════════════════════════════════════════ */

static unsigned reg_slot(uint32_t id) { return (id * 2654435761u) >> 22; }

static flux_download_t *reg_find_locked(flux_registry_t *reg, uint32_t id) {
    for (flux_download_t *d = reg->buckets[reg_slot(id)]; d; d = d->hash_next)
        if (d->id == id) return d;
    return NULL;
}

static void reg_insert_locked(flux_registry_t *reg, flux_download_t *dl) {
    unsigned s = reg_slot(dl->id);
    dl->hash_next    = reg->buckets[s];
    reg->buckets[s]  = dl;
    reg->count++;
}

static void reg_remove_locked(flux_registry_t *reg, flux_download_t *dl) {
    flux_download_t **pp = &reg->buckets[reg_slot(dl->id)];
    while (*pp && *pp != dl) pp = &(*pp)->hash_next;
    if (*pp) { *pp = dl->hash_next; reg->count--; }
    dl->hash_next = NULL;
}

/* Join (the worker has exited) and free. Registry lock held. */
static void dl_retire_locked(flux_registry_t *reg, flux_download_t *dl) {
    reg_remove_locked(reg, dl);
    if (dl->thread_started && !dl->joined) { pthread_join(dl->thread, NULL); dl->joined = 1; }
    dl_free(dl);
}

int flux_registry_init(flux_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));
    pthread_mutex_init(&reg->lock, NULL);
    flux_bw_init(&reg->global_bw, 0);                 /* unlimited until told otherwise */
    flux_bw_set_burst_ms(&reg->global_bw, DL_BW_BURST_MS);
    return flux_tls_global_init();
}

flux_download_t *flux_registry_find(flux_registry_t *reg, uint32_t id) {
    pthread_mutex_lock(&reg->lock);
    flux_download_t *dl = reg_find_locked(reg, id);
    pthread_mutex_unlock(&reg->lock);
    return dl;
}

void flux_registry_foreach(flux_registry_t *reg,
                           void (*fn)(flux_download_t *dl, void *ctx), void *ctx) {
    pthread_mutex_lock(&reg->lock);
    for (unsigned b = 0; b < FLUX_REG_BUCKETS; b++)
        for (flux_download_t *d = reg->buckets[b]; d; d = d->hash_next) fn(d, ctx);
    pthread_mutex_unlock(&reg->lock);
}

void flux_registry_reap(flux_registry_t *reg, uint64_t now_ms) {
    pthread_mutex_lock(&reg->lock);
    for (unsigned b = 0; b < FLUX_REG_BUCKETS; b++) {
        flux_download_t **pp = &reg->buckets[b];
        while (*pp) {
            flux_download_t *dl = *pp;
            int retire = 0;

            if (ALOAD(&dl->finished)) {
                dl_state_t s = get_state(dl);
                if (s == DL_PAUSED) {
                    /* kept until it is resumed or cancelled: a paused download
                     * is not "finished" from the user's point of view */
                } else if (s == DL_CANCELLED) {
                    retire = 1;
                } else {
                    if (dl->finished_at_ms == 0)
                        dl->finished_at_ms = now_ms ? now_ms : 1;
                    else if (now_ms - dl->finished_at_ms >= FLUX_DL_RETAIN_MS)
                        retire = 1;
                }
            }

            if (retire) {
                *pp = dl->hash_next;
                reg->count--;
                dl->hash_next = NULL;
                if (dl->thread_started && !dl->joined) { pthread_join(dl->thread, NULL); dl->joined = 1; }
                dl_free(dl);
            } else {
                pp = &dl->hash_next;
            }
        }
    }
    pthread_mutex_unlock(&reg->lock);
}

void flux_registry_destroy(flux_registry_t *reg) {
    int stuck = 0;

    /* Ask every live worker to stop (keeping partial files) ... */
    pthread_mutex_lock(&reg->lock);
    for (unsigned b = 0; b < FLUX_REG_BUCKETS; b++)
        for (flux_download_t *d = reg->buckets[b]; d; d = d->hash_next)
            if (dl_alive(d)) { ASTORE(&d->stop_mode, DL_STOP_KEEP); wake_worker(d); }
    pthread_mutex_unlock(&reg->lock);

    /* ... then collect them. A worker blocked in DNS may take a while; the
     * process is exiting anyway, so after a grace period it is left behind
     * rather than freed under its feet. */
    pthread_mutex_lock(&reg->lock);
    for (unsigned b = 0; b < FLUX_REG_BUCKETS; b++) {
        flux_download_t *d = reg->buckets[b];
        while (d) {
            flux_download_t *next = d->hash_next;
            if (dl_alive(d) && !wait_finished(d, STOP_WAIT_MS)) {
                stuck++;
            } else {
                reg_remove_locked(reg, d);
                if (d->thread_started && !d->joined) { pthread_join(d->thread, NULL); d->joined = 1; }
                dl_free(d);
            }
            d = next;
        }
    }
    pthread_mutex_unlock(&reg->lock);

    if (stuck == 0) {
        flux_bw_destroy(&reg->global_bw);
        pthread_mutex_destroy(&reg->lock);
        flux_tls_global_cleanup();
    }
}

/* ═══ Internal API used by segment.c ══════════════════════════════════════ */

void flux_dl_add_progress(flux_download_t *dl, int64_t delta) {
    /* Negative deltas (bytes thrown away on a restart) wrap correctly. */
    __atomic_fetch_add(&dl->downloaded_bytes, (uint64_t)delta, __ATOMIC_RELAXED);
}

uint32_t flux_dl_charge(flux_download_t *dl, size_t bytes) {
    uint32_t a = flux_bw_charge(&dl->bw_bucket, bytes);
    uint32_t b = dl->global_bucket ? flux_bw_charge(dl->global_bucket, bytes) : 0u;
    return a > b ? a : b;
}

size_t flux_dl_read_cap(flux_download_t *dl) {
    uint64_t r1   = flux_bw_rate(&dl->bw_bucket);
    uint64_t r2   = dl->global_bucket ? flux_bw_rate(dl->global_bucket) : 0u;
    uint64_t rate = (r1 && r2) ? flux_min_u64(r1, r2) : (r1 ? r1 : r2);
    if (rate == 0) return SIZE_MAX;
    /* Reads of about an eighth of a second's worth keep the limiter smooth
     * without making tiny syscalls at high rates. */
    return (size_t)flux_max_u64(4096u, flux_min_u64(rate / 8u, 65536u));
}

int flux_dl_note_redirect(flux_download_t *dl, const char *location) {
    if (dl->redirects >= HTTP_MAX_REDIRECTS) {
        flux_set_error(FLUX_ERR_HTTP_REDIR, "too many redirects (more than %d)", HTTP_MAX_REDIRECTS);
        return -1;
    }
    http_url_t next;
    if (http_url_resolve(&dl->target, location, &next) < 0) {
        flux_set_error(FLUX_ERR_BAD_URL, "redirect to an invalid or unsupported location");
        return -1;
    }
    dl->redirects++;

    int same_server = strcmp(next.host, dl->target.host) == 0 &&
                      next.port == dl->target.port &&
                      strcmp(next.scheme, dl->target.scheme) == 0;
    flux_log(2, "download %u: redirect to %s://%s:%u", dl->id, next.scheme, next.host, (unsigned)next.port);

    dl->target  = next;
    dl->use_tls = (uint8_t)http_url_is_tls(&next);
    if (!same_server) dl->addr_len = 0;               /* a new host needs a new address */
    dl->target_gen++;
    return 0;
}

/* ═══ Snapshot ════════════════════════════════════════════════════════════ */

uint32_t flux_dl_eta(const flux_download_t *dl) {
    if (get_state(dl) != DL_RUNNING) return UINT32_MAX;
    uint64_t total = ALOAD(&dl->total_size);
    uint64_t got   = ALOAD(&dl->downloaded_bytes);
    uint32_t speed = ALOAD(&dl->speed_bps);
    if (total == 0 || speed == 0) return UINT32_MAX;
    if (got >= total) return 0;
    uint64_t eta = (total - got) / speed;
    return eta >= UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)eta;
}

void flux_dl_snapshot(const flux_download_t *dl, flux_rsp_status_t *out) {
    memset(out, 0, sizeof(*out));
    uint64_t total = ALOAD(&dl->total_size);
    uint64_t got   = ALOAD(&dl->downloaded_bytes);
    dl_state_t st  = get_state(dl);

    if (st == DL_COMPLETE && total) got = total;
    if (total && got > total)       got = total;       /* bytes re-fetched after a restart */

    out->id               = dl->id;
    out->status           = (uint8_t)st;
    out->active_segments  = ALOAD(&dl->num_segs_active);
    out->total_segments   = ALOAD(&dl->num_segs);
    out->total_bytes      = total;
    out->downloaded_bytes = got;
    out->speed_bps        = st == DL_RUNNING ? ALOAD(&dl->speed_bps) : 0u;
    out->eta_seconds      = flux_dl_eta(dl);
}

/* ═══ Probe ═══════════════════════════════════════════════════════════════ */

typedef struct {
    http_response_t         resp;
    flux_conn_t            *conn;       /* the connection the response arrived on */
    struct sockaddr_storage addr;       /* the peer that answered                 */
    socklen_t               addr_len;
} probe_t;

typedef struct {
    struct sockaddr_storage ss;
    socklen_t               len;
} addr_t;

/* Outcome of one probe attempt */
enum { PR_OK = 0, PR_INTERRUPTED = 1, PR_FATAL = -1, PR_TRANSIENT = -2 };
/* Outcome of dl_probe() */
enum { PROBE_OK = 0, PROBE_INTERRUPTED = 1, PROBE_FAILED = -1 };

/* Block until `fd` is ready for `events`, `deadline_ms` passes, or a pause/stop
 * request arrives. 1 = ready, 0 = timed out, -1 = interrupted. fd < 0 just
 * sleeps (interruptibly). */
static int wait_io(flux_download_t *dl, int fd, short events, uint64_t deadline_ms) {
    for (;;) {
        if (interrupted(dl)) return -1;
        uint64_t now = flux_now_ms();
        if (now >= deadline_ms) return 0;
        uint64_t left = deadline_ms - now;

        struct pollfd p[2] = {
            { .fd = fd,          .events = events },
            { .fd = dl->wake_fd, .events = POLLIN },
        };
        /* Cap each wait at 1 s: even a wake-up that raced past us can only
         * delay a pause by that much. */
        int n = poll(p, 2, (int)(left > 1000u ? 1000u : left));
        if (n < 0) { if (errno == EINTR) continue; return 0; }
        if (p[1].revents & POLLIN) { drain_wake(dl); continue; }
        if (n > 0 && p[0].revents) return 1;          /* IN/OUT/ERR/HUP: the I/O call reports which */
    }
}

static void probe_close(probe_t *p) {
    if (p->conn) { flux_conn_close(p->conn); p->conn = NULL; }
}

/* Resolve host:port. IPv4 comes first: a dead IPv6 route is far more common
 * than a dead IPv4 one, and one connect that has to time out costs 10 s. */
static int resolve_host(const char *host, uint16_t port, addr_t *out, int *transient) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char ports[8];
    snprintf(ports, sizeof(ports), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, ports, &hints, &res);
    if (rc != 0) {
        *transient = (rc == EAI_AGAIN || rc == EAI_SYSTEM);
        flux_set_error(FLUX_ERR_DNS, "cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        int family = pass == 0 ? AF_INET : AF_INET6;
        for (struct addrinfo *ai = res; ai && n < MAX_ADDRS; ai = ai->ai_next) {
            if (ai->ai_family != family || ai->ai_addrlen > sizeof(out[n].ss)) continue;
            memcpy(&out[n].ss, ai->ai_addr, ai->ai_addrlen);
            out[n].len = ai->ai_addrlen;
            n++;
        }
    }
    freeaddrinfo(res);

    if (n == 0) {
        *transient = 0;
        flux_set_error(FLUX_ERR_DNS, "no usable address for %s", host);
        return -1;
    }
    return n;
}

/* TCP connect + TLS handshake with interruptible waits.
 * 1 = ready, 0 = failed (error set), -1 = interrupted. */
static int connect_ready(flux_download_t *dl, flux_conn_t *c, unsigned budget_ms) {
    uint64_t deadline = flux_now_ms() + budget_ms;

    if (flux_conn_start_connect(c) < 0) return 0;
    int w = wait_io(dl, c->fd, POLLOUT, deadline);
    if (w < 0) return -1;
    if (w == 0) { flux_set_error(FLUX_ERR_TIMEOUT_, "connect to %s:%u timed out", c->host, (unsigned)c->port); return 0; }

    int rc = flux_conn_finish_connect(c);
    while (rc == 0 || rc == 2) {                      /* TLS needs more round trips */
        w = wait_io(dl, c->fd, rc == 2 ? POLLOUT : POLLIN, deadline);
        if (w < 0) return -1;
        if (w == 0) { flux_set_error(FLUX_ERR_TIMEOUT_, "TLS handshake with %s timed out", c->host); return 0; }
        rc = flux_conn_tls_handshake_continue(c);
    }
    return rc == 1 ? 1 : 0;
}

/* One complete probe round trip: resolve, connect, send, read the response head.
 * On PR_OK, p->conn is a live connection positioned just after the head. */
static int probe_attempt(flux_download_t *dl, const http_url_t *u, int ranged, probe_t *p) {
    p->conn = NULL;
    memset(&p->resp, 0, sizeof(p->resp));

    addr_t addrs[MAX_ADDRS];
    int    transient = 0;
    int    naddr = resolve_host(u->host, u->port, addrs, &transient);
    if (naddr < 0) return transient ? PR_TRANSIENT : PR_FATAL;
    if (interrupted(dl)) return PR_INTERRUPTED;

    int          use_tls = http_url_is_tls(u);
    flux_conn_t *c       = NULL;
    int          chosen  = 0;

    for (int i = 0; i < naddr && !c; i++) {
        flux_conn_t *cand = flux_conn_new(u->host, u->port, use_tls);
        if (!cand) return PR_FATAL;
        flux_conn_set_addr(cand, (const struct sockaddr *)&addrs[i].ss, addrs[i].len);

        int rc = connect_ready(dl, cand, i + 1 < naddr ? PROBE_FIRST_ADDR_MS : FLUX_CONN_CONNECT_TOMS);
        if (rc == 1) { c = cand; chosen = i; continue; }

        flux_conn_close(cand);
        if (rc < 0) return PR_INTERRUPTED;
        if (flux_last_error() == FLUX_ERR_TLS_CERT) return PR_FATAL;   /* will not get better */
    }
    if (!c) return PR_TRANSIENT;                      /* the last candidate's error is still set */

    /* ── request ── */
    char req[FLUX_MAX_URL + 1536];
    int  req_len = http_build_get(req, sizeof(req), u, ranged, 0, 0);
    if (req_len < 0) {
        flux_conn_close(c);
        flux_set_error(FLUX_ERR_BAD_URL, "request does not fit its buffer (URL too long)");
        return PR_FATAL;
    }

    uint64_t deadline = flux_now_ms() + FLUX_CONN_SEND_TOMS;
    size_t   sent     = 0;
    while (sent < (size_t)req_len) {
        ssize_t n = flux_conn_send(c, req + sent, (size_t)req_len - sent);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && flux_last_error() == FLUX_ERR_WOULDBLOCK) {
            int w = wait_io(dl, c->fd, POLLOUT, deadline);
            if (w < 0) { flux_conn_close(c); return PR_INTERRUPTED; }
            if (w == 0) { flux_conn_close(c); flux_set_error(FLUX_ERR_TIMEOUT_, "sending the request timed out"); return PR_TRANSIENT; }
            continue;
        }
        flux_conn_close(c);
        return PR_TRANSIENT;
    }

    /* ── response head ── */
    deadline = flux_now_ms() + FLUX_CONN_RECV_TOMS;
    for (;;) {
        int hr = flux_http_read_head(c, &p->resp);
        if (hr < 0) { flux_conn_close(c); return PR_TRANSIENT; }
        if (hr == 1) break;

        ssize_t n = flux_conn_fill(c);
        if (n > 0) continue;
        if (n == 0) {
            flux_conn_close(c);
            flux_set_error(FLUX_ERR_NETWORK, "connection closed before the response headers arrived");
            return PR_TRANSIENT;
        }
        if (flux_last_error() == FLUX_ERR_WOULDBLOCK) {
            int w = wait_io(dl, c->fd, POLLIN, deadline);
            if (w < 0) { flux_conn_close(c); return PR_INTERRUPTED; }
            if (w == 0) {
                flux_conn_close(c);
                flux_set_error(FLUX_ERR_TIMEOUT_, "no response from %s within %u s", u->host,
                               (unsigned)(FLUX_CONN_RECV_TOMS / 1000));
                return PR_TRANSIENT;
            }
            continue;
        }
        flux_conn_close(c);
        return PR_TRANSIENT;
    }

    p->conn     = c;
    p->addr     = addrs[chosen].ss;
    p->addr_len = addrs[chosen].len;
    return PR_OK;
}

/* A probe attempt with retries. Transient failures (network errors, 408/425/5xx)
 * get PROBE_ATTEMPTS tries. A server that answers 429 / 503 is busy, not broken:
 * it is waited for — as long as the patience lasts, and the host's gate hears of
 * it so the segments that follow do not pile on. PR_OK means "we have a
 * response" — the caller judges the status. */
static int probe_fetch(flux_download_t *dl, const http_url_t *u, int ranged, probe_t *p) {
    unsigned delay         = PROBE_BACKOFF_MS;
    uint64_t refused_since = 0;
    unsigned refusals      = 0;

    for (int attempt = 1; ; attempt++) {
        /* Another download may have been told to leave this host alone for a while. */
        uint32_t pause = flux_gate_pause_ms(u->host, u->port, flux_now_ms());
        if (pause > flux_gate_patience_ms()) pause = flux_gate_patience_ms();
        if (pause > 0 && wait_io(dl, -1, 0, flux_now_ms() + pause) < 0) return PR_INTERRUPTED;

        int rc = probe_attempt(dl, u, ranged, p);

        if (rc == PR_OK && http_status_is_pushback(p->resp.status)) {
            uint64_t now      = flux_now_ms();
            uint32_t after_ms = p->resp.retry_after_s * 1000u;
            uint32_t lowered  = flux_gate_note_refusal(u->host, u->port, now, after_ms);
            if (lowered)
                flux_log(1, "%s:%u is refusing connections (HTTP %d) — using at most %u at a time for now",
                         u->host, (unsigned)u->port, p->resp.status, (unsigned)lowered);

            if (refused_since == 0) refused_since = now;
            uint64_t waited   = now - refused_since;
            uint32_t patience = flux_gate_patience_ms();
            if (waited >= patience || after_ms > patience - waited) return PR_OK;   /* out of patience: the caller reports the status */

            uint32_t wait = flux_gate_backoff_ms(refusals++, dl->id);
            if (after_ms > wait) wait = after_ms;
            if (wait > patience - waited) wait = (uint32_t)(patience - waited);
            flux_log(2, "download %u: probe got HTTP %d, asking again in %u ms", dl->id, p->resp.status, wait);
            probe_close(p);
            if (wait_io(dl, -1, 0, flux_now_ms() + wait) < 0) return PR_INTERRUPTED;
            attempt--;                          /* pushback does not use up the attempts kept for real failures */
            continue;
        }

        if (rc == PR_OK) {
            if (!http_status_is_retryable(p->resp.status) || attempt >= PROBE_ATTEMPTS) return PR_OK;
            unsigned wait = delay;
            unsigned ra   = p->resp.retry_after_s > PROBE_RETRY_AFTER_MAX_MS / 1000u
                          ? PROBE_RETRY_AFTER_MAX_MS : p->resp.retry_after_s * 1000u;
            if (ra > wait) wait = ra;
            flux_log(2, "download %u: probe got HTTP %d, retrying in %u ms", dl->id, p->resp.status, wait);
            probe_close(p);
            if (wait_io(dl, -1, 0, flux_now_ms() + wait) < 0) return PR_INTERRUPTED;
        } else if (rc == PR_TRANSIENT && attempt < PROBE_ATTEMPTS) {
            flux_log(2, "download %u: probe attempt %d failed (%s), retrying in %u ms",
                     dl->id, attempt, flux_last_error_str(), delay);
            if (wait_io(dl, -1, 0, flux_now_ms() + delay) < 0) return PR_INTERRUPTED;
        } else {
            return rc == PR_INTERRUPTED ? PR_INTERRUPTED : PR_FATAL;
        }
        delay *= 2;
    }
}

static int is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/* Identity of the remote file: Last-Modified, else a strong ETag, mixed with
 * the size. 0 means "the server told us nothing to compare". */
static uint64_t make_validator(const http_response_t *r, uint64_t total) {
    const char *s = NULL;
    if (r->last_modified[0])                                            s = r->last_modified;
    else if (r->etag[0] && !(r->etag[0] == 'W' && r->etag[1] == '/'))   s = r->etag;
    if (!s) return 0;

    uint64_t h = 1469598103934665603ULL;               /* FNV-1a */
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
    h ^= total * 0x9E3779B97F4A7C15ULL;
    return h ? h : 1u;
}

/* Discover the file: size, range support, validators, final URL, peer address. */
static int dl_probe(flux_download_t *dl) {
    http_url_t cur    = dl->origin;
    int        ranged = 1;
    int        hops   = 0;
    probe_t    p;
    memset(&p, 0, sizeof(p));

    for (;;) {
        int rc = probe_fetch(dl, &cur, ranged, &p);
        if (rc == PR_INTERRUPTED) return PROBE_INTERRUPTED;
        if (rc != PR_OK)          return PROBE_FAILED;

        int st = p.resp.status;

        if (is_redirect(st)) {
            if (p.resp.location[0] == '\0') {
                probe_close(&p);
                flux_set_error(FLUX_ERR_HTTP_REDIR, "HTTP %d without a Location header", st);
                return PROBE_FAILED;
            }
            if (++hops > HTTP_MAX_REDIRECTS) {
                probe_close(&p);
                flux_set_error(FLUX_ERR_HTTP_REDIR, "too many redirects (more than %d)", HTTP_MAX_REDIRECTS);
                return PROBE_FAILED;
            }
            http_url_t next;
            if (http_url_resolve(&cur, p.resp.location, &next) < 0) {
                probe_close(&p);
                flux_set_error(FLUX_ERR_BAD_URL, "redirect to an invalid or unsupported location");
                return PROBE_FAILED;
            }
            probe_close(&p);
            cur = next;
            continue;
        }

        /* A server that chokes on the Range header (or reports an empty file as
         * 416) gets one plain GET before we give up. */
        if (ranged && (st == 400 || st == 405 || st == 416 || st == 501)) {
            probe_close(&p);
            ranged = 0;
            continue;
        }
        break;
    }

    const http_response_t *r  = &p.resp;
    int                    st = r->status;

    if (st != 200 && st != 206) {
        flux_error_t code = (st < 200 || st >= 300) ? http_status_to_flux_error(st) : FLUX_ERR_HTTP_STATUS;
        if (http_status_is_pushback(st))
            flux_set_error(code, "HTTP %d %s (the server kept refusing; try again later)", st, r->reason);
        else
            flux_set_error(code, "HTTP %d %s", st, r->reason);
        probe_close(&p);
        return PROBE_FAILED;
    }

    uint64_t total  = 0;
    int      ranges = 0;
    int      empty  = 0;

    if (st == 206) {
        /* Content-Range: bytes 0-0/TOTAL */
        if (r->has_content_range && !r->content_range_unsat &&
            r->content_range_start == 0 && r->content_range_total > 0) {
            total  = r->content_range_total;
            ranges = 1;
        }
        /* A 206 we cannot make sense of is treated like a server without ranges. */
    } else {
        if (r->has_content_length && !r->chunked) total = r->content_length;
        empty = r->has_content_length && !r->chunked && r->content_length == 0;
    }

    if (total > (uint64_t)INT64_MAX) {
        probe_close(&p);
        flux_set_error(FLUX_ERR_HTTP_PARSE, "server reports an impossible file size");
        return PROBE_FAILED;
    }

    dl->addr         = p.addr;
    dl->addr_len     = p.addr_len;
    dl->target       = cur;
    dl->use_tls      = (uint8_t)http_url_is_tls(&cur);
    dl->target_gen++;
    dl->redirects    = 0;
    dl->accept_ranges = (uint8_t)ranges;
    dl->empty        = (uint8_t)empty;
    dl->validator    = make_validator(r, total);
    ASTORE(&dl->total_size, total);

    if (st == 200 && !empty) {
        /* The server ignored Range: this response IS the whole file, and the
         * connection is already warm — hand it to the single segment instead
         * of paying for a second connect + TLS handshake. */
        dl->probe_resp = malloc(sizeof(*dl->probe_resp));
        if (dl->probe_resp) {
            *dl->probe_resp = p.resp;
            dl->probe_conn  = p.conn;
            p.conn          = NULL;
        }
    }
    probe_close(&p);

    flux_log(1, "download %u: %s — %s, %s", dl->id, log_url(dl),
             total ? "size known" : "size unknown",
             ranges ? "ranges supported" : "single stream (no ranges)");
    return PROBE_OK;
}

/* ═══ Output file and segment layout ══════════════════════════════════════ */

/* mkdir -p for the directory part of `path`. */
static int make_parent_dirs(const char *path) {
    char buf[FLUX_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);

    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;              /* no directory part, or the root */
    *slash = '\0';

    for (char *q = buf + 1; ; q++) {
        if (*q != '/' && *q != '\0') continue;
        char saved = *q;
        *q = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST) {
            int e = errno;
            flux_set_error(e == ENOSPC || e == EDQUOT ? FLUX_ERR_DISK_SPACE : FLUX_ERR_DISK_OPEN,
                           "cannot create directory %s: %s", buf, strerror(e));
            return -1;
        }
        *q = saved;
        if (saved == '\0') break;
    }
    return 0;
}

static int open_data_file(flux_download_t *dl, int fresh) {
    int flags = O_RDWR | O_CLOEXEC | (fresh ? (O_CREAT | O_TRUNC) : 0);
    int fd    = open(dl->file_path, flags, 0644);
    if (fd < 0) {
        int e = errno;
        flux_set_error(e == ENOSPC || e == EDQUOT ? FLUX_ERR_DISK_SPACE : FLUX_ERR_DISK_OPEN,
                       "cannot open %s: %s", dl->file_path, strerror(e));
        return -1;
    }
    dl->file_fd   = fd;
    dl->owns_file = 1;
    return 0;
}

/* Reserve the whole file up front: fails early and cleanly when the disk is too
 * small, and keeps the file contiguous. Uses fallocate(2) directly — glibc's
 * posix_fallocate() silently falls back to writing zeros block by block on
 * file systems without support, which can take minutes for a large file. */
static int preallocate(int fd, uint64_t size) {
    if (size == 0) return 0;
    if (fallocate(fd, 0, 0, (off_t)size) == 0) return 0;

    int e = errno;
    if (e == ENOSPC || e == EDQUOT) {
        flux_set_error(FLUX_ERR_DISK_SPACE, "not enough disk space for %llu bytes", (unsigned long long)size);
        return -1;
    }
    if (e == EFBIG) {
        flux_set_error(FLUX_ERR_DISK_WRITE, "a file of %llu bytes is too large for this file system",
                       (unsigned long long)size);
        return -1;
    }
    /* EOPNOTSUPP / ENOSYS / EINVAL / EPERM: no native preallocation here. A
     * sparse file of the right size is just as usable. */
    if (ftruncate(fd, (off_t)size) < 0) {
        flux_set_error(FLUX_ERR_DISK_WRITE, "cannot size the output file: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* Length of `url` up to (not including) its query or fragment. */
static size_t url_base_len(const char *url) { return strcspn(url, "?#"); }

/* Can the on-disk partial download described by `r` be continued with the file
 * we just probed? On "no", *why names the reason. */
static int resume_matches(const flux_download_t *dl, const flux_resume_t *r, const char **why) {
    uint64_t total = ALOAD(&dl->total_size);

    if (!dl->accept_ranges || total == 0)  { *why = "the server cannot resume";      return 0; }
    if (r->hdr.total_size != total)        { *why = "the file size changed";        return 0; }

    int validators = r->hdr.validator != 0 && dl->validator != 0;
    if (validators && r->hdr.validator != dl->validator) { *why = "the file changed on the server"; return 0; }

    if (strcmp(r->url, dl->url) != 0) {
        /* A refreshed signed URL differs only in its query: accept that, but only
         * when the validators vouch for it. Anything else is another file. */
        size_t a = url_base_len(r->url), b = url_base_len(dl->url);
        if (!validators || a != b || strncmp(r->url, dl->url, a) != 0) {
            *why = "it was started from a different URL";
            return 0;
        }
    }

    unsigned n = r->hdr.num_segments;
    if (n < 1 || n > FLUX_DL_MAX_SEGMENTS) { *why = "the resume file is malformed"; return 0; }

    uint64_t expect = 0;
    for (unsigned i = 0; i < n; i++) {
        const flux_resume_seg_t *s = &r->segs[i];
        if (s->range_start != expect || s->range_end < s->range_start || s->range_end >= total) {
            *why = "the resume file does not cover the file";
            return 0;
        }
        if (s->written > s->range_end - s->range_start + 1u) { *why = "the resume file is inconsistent"; return 0; }
        expect = s->range_end + 1u;
    }
    if (expect != total) { *why = "the resume file does not cover the file"; return 0; }

    struct stat sb;
    if (stat(dl->file_path, &sb) != 0 || !S_ISREG(sb.st_mode) || (uint64_t)sb.st_size != total) {
        *why = "the partial file is missing or has the wrong size";
        return 0;
    }
    return 1;
}

static int add_segment(flux_download_t *dl, unsigned index, uint64_t start, uint64_t end, uint64_t written) {
    flux_segment_t *seg = flux_seg_new(dl, (uint8_t)index, start, end);
    if (!seg) return -1;
    seg->written = written;
    dl->segs[index] = seg;
    return 0;
}

/* Everything between "we know what the file is" and "start downloading":
 * resume decision, output file, pre-allocation, segment layout. */
static int dl_prepare(flux_download_t *dl) {
    uint64_t total = ALOAD(&dl->total_size);

    if (make_parent_dirs(dl->file_path) < 0) return -1;

    dl->resume = calloc(1, sizeof(*dl->resume));
    if (!dl->resume) { flux_set_error(FLUX_ERR_NOMEM, "cannot allocate resume state"); return -1; }
    dl->saved_written = UINT64_MAX;                   /* the first checkpoint always writes */

    if (dl->empty) {                                  /* nothing to transfer: just create the file */
        remove_resume_files(dl);
        return open_data_file(dl, 1);
    }

    /* ── resume? ── */
    unsigned segs_used = 0;
    if (flux_resume_load(dl->resume, dl->part_path) == 0) {
        const char *why = NULL;
        if (resume_matches(dl, dl->resume, &why)) {
            if (open_data_file(dl, 0) < 0) return -1;
            segs_used = dl->resume->hdr.num_segments;
            uint64_t have = 0;
            for (unsigned i = 0; i < segs_used; i++) {
                const flux_resume_seg_t *rs = &dl->resume->segs[i];
                if (add_segment(dl, i, rs->range_start, rs->range_end, rs->written) < 0) return -1;
                have += rs->written;
            }
            ASTORE(&dl->downloaded_bytes, have);
            flux_log(1, "download %u: resuming — %llu of %llu bytes already on disk",
                     dl->id, (unsigned long long)have, (unsigned long long)total);
        } else {
            flux_log(1, "download %u: starting over — %s", dl->id, why);
        }
    }

    /* ── fresh start ── */
    if (segs_used == 0) {
        remove_resume_files(dl);
        if (open_data_file(dl, 1) < 0) return -1;
        if (preallocate(dl->file_fd, total) < 0) return -1;
        ASTORE(&dl->downloaded_bytes, 0);

        unsigned n = dl->want_segs ? dl->want_segs : DEFAULT_SEGMENTS;
        if (!dl->accept_ranges || total == 0) {
            n = 1;
        } else {
            uint64_t by_size = total / FLUX_MIN_SEGMENT_BYTES;
            if (by_size < 1) by_size = 1;
            if (n > by_size) n = (unsigned)by_size;
        }
        if (n > FLUX_DL_MAX_SEGMENTS) n = FLUX_DL_MAX_SEGMENTS;

        uint64_t chunk = n > 1 ? total / n : total;
        if (n > 1 && chunk >= ALIGN_SEG) chunk &= ~(uint64_t)(ALIGN_SEG - 1u);

        for (unsigned i = 0; i < n; i++) {
            uint64_t start = (uint64_t)i * chunk;
            uint64_t end   = i + 1 == n ? (total ? total - 1u : UINT64_MAX) : start + chunk - 1u;
            if (add_segment(dl, i, start, end, 0) < 0) return -1;
        }
        segs_used = n;
    }

    ASTORE(&dl->num_segs, (uint8_t)segs_used);
    return 0;
}

/* ═══ Checkpointing ═══════════════════════════════════════════════════════ */

static void save_resume(flux_download_t *dl) {
    flux_resume_t *r = dl->resume;
    if (!r || !dl->accept_ranges || ALOAD(&dl->total_size) == 0) return;   /* nothing resumable */

    memset(&r->hdr, 0, sizeof(r->hdr));
    r->hdr.magic         = FLUX_RESUME_MAGIC;
    r->hdr.version       = FLUX_RESUME_VER;
    r->hdr.download_id   = dl->id;
    r->hdr.num_segments  = dl->num_segs;
    r->hdr.total_size    = ALOAD(&dl->total_size);
    r->hdr.checksum_type = dl->cksum_type;
    memcpy(r->hdr.checksum, dl->cksum_expected, sizeof(r->hdr.checksum));
    r->hdr.validator     = dl->validator;
    snprintf(r->url,  sizeof(r->url),  "%s", dl->url);
    snprintf(r->path, sizeof(r->path), "%s", dl->file_path);

    for (unsigned i = 0; i < dl->num_segs; i++) {
        const flux_segment_t *s  = dl->segs[i];
        flux_resume_seg_t    *rs = &r->segs[i];
        memset(rs, 0, sizeof(*rs));
        rs->range_start = s->range_start;
        rs->range_end   = s->range_end;
        rs->written     = s->written;
        rs->status      = s->state == SEG_COMPLETE ? 1 : 0;
    }
    if (flux_resume_save(r, dl->part_path) < 0)
        flux_log(1, "download %u: cannot save resume state: %s", dl->id, flux_last_error_str());
}

/* Make everything flushed so far durable, then record it. The order matters:
 * the resume file may only ever claim bytes that are really on disk, or a
 * power cut would turn "resume" into silent corruption.
 * Returns -1 (error set) if the disk reported a write failure. */
static int checkpoint(flux_download_t *dl) {
    if (dl->file_fd >= 0 && fdatasync(dl->file_fd) < 0) {
        int e = errno;
        if (e == EIO || e == ENOSPC || e == EDQUOT) {
            flux_set_error(e == EIO ? FLUX_ERR_DISK_WRITE : FLUX_ERR_DISK_SPACE,
                           "writing to disk failed: %s", strerror(e));
            return -1;
        }
        /* EINVAL / EROFS / ENOTSUP: this file system cannot sync; carry on. */
    }
    uint64_t on_disk = bytes_on_disk(dl);
    if (on_disk != dl->saved_written) {
        save_resume(dl);
        dl->saved_written = on_disk;
    }
    dl->last_resume_save_ms = flux_now_ms();
    return 0;
}

/* ═══ Endings ═════════════════════════════════════════════════════════════ */

static int halt_segments(flux_download_t *dl) {
    int rc = 0;
    for (unsigned i = 0; i < dl->num_segs; i++)
        if (dl->segs[i] && flux_seg_halt(dl->segs[i]) < 0) rc = -1;
    return rc;
}

/* The download failed: keep what is on disk (a retry resumes from it) unless
 * `keep_progress` is 0, record why, and tell the client. */
static void finish_fail(flux_download_t *dl, flux_error_t code, const char *msg, int keep_progress) {
    char text[sizeof(dl->err_msg)];
    snprintf(text, sizeof(text), "%s", msg && *msg ? msg : flux_strerror(code));   /* before anything else touches the error store */

    halt_segments(dl);
    if (keep_progress && dl->resume && dl->file_fd >= 0) (void)checkpoint(dl);

    dl->err_code    = code;
    memcpy(dl->err_msg, text, sizeof(text));
    dl->finished_ms = flux_now_ms();
    ASTORE(&dl->speed_bps, 0u);
    ASTORE(&dl->num_segs_active, 0);
    set_state(dl, DL_ERROR);
    flux_log(1, "download %u failed: %s", dl->id, dl->err_msg);
    emit(dl, FLUX_RSP_ERROR);
}

/* Stop with progress kept. `announce`: tell clients (pause) or not (shutdown). */
static void finish_halt(flux_download_t *dl, int announce) {
    int rc = halt_segments(dl);
    if (rc == 0 && dl->resume) rc = checkpoint(dl);
    if (rc < 0) { finish_fail(dl, flux_last_error(), flux_last_error_str(), 1); return; }

    ASTORE(&dl->speed_bps, 0u);
    ASTORE(&dl->num_segs_active, 0);
    set_state(dl, DL_PAUSED);
    flux_log(1, "download %u paused at %llu bytes", dl->id, (unsigned long long)bytes_on_disk(dl));
    if (announce) emit(dl, FLUX_RSP_PROGRESS);
}

static void finish_stop(flux_download_t *dl) {
    if (ALOAD(&dl->stop_mode) != DL_STOP_DELETE) { finish_halt(dl, 0); return; }

    for (unsigned i = 0; i < dl->num_segs; i++)
        if (dl->segs[i]) flux_seg_abort(dl->segs[i]);
    if (dl->file_fd >= 0) { close(dl->file_fd); dl->file_fd = -1; }
    if (dl->owns_file) unlink(dl->file_path);        /* never a file we did not create */
    remove_resume_files(dl);

    dl->finished_ms = flux_now_ms();
    ASTORE(&dl->speed_bps, 0u);
    ASTORE(&dl->num_segs_active, 0);
    set_state(dl, DL_CANCELLED);
    flux_log(1, "download %u cancelled", dl->id);
}

/* SHA-256 / MD5 of the finished file, in blocks so a pause or cancel is noticed
 * promptly. 0 = ok, 1 = interrupted, -1 = error. */
static int hash_data_file(flux_download_t *dl, uint64_t size, uint8_t *out, size_t *out_len) {
    flux_cksum_ctx_t *ctx = flux_cksum_new((flux_cksum_type_t)dl->cksum_type);
    if (!ctx) return -1;
    uint8_t *buf = malloc(HASH_BLOCK);
    if (!buf) { flux_cksum_free(ctx); flux_set_error(FLUX_ERR_NOMEM, "cannot allocate checksum buffer"); return -1; }

    int      rc  = 0;
    uint64_t off = 0;
    while (off < size) {
        if (interrupted(dl)) { rc = 1; break; }
        size_t  want = (size - off) > HASH_BLOCK ? HASH_BLOCK : (size_t)(size - off);
        ssize_t n    = pread(dl->file_fd, buf, want, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            flux_set_error(FLUX_ERR_DISK_READ, "cannot read the file to verify it: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (n == 0) {
            flux_set_error(FLUX_ERR_DISK_READ, "the file is shorter than expected (%llu of %llu bytes)",
                           (unsigned long long)off, (unsigned long long)size);
            rc = -1;
            break;
        }
        if (flux_cksum_update(ctx, buf, (size_t)n) < 0) { rc = -1; break; }
        off += (uint64_t)n;
    }
    if (rc == 0 && flux_cksum_final(ctx, out, out_len) < 0) rc = -1;

    free(buf);
    flux_cksum_free(ctx);
    return rc;
}

/* Every segment has all its bytes: seal the file, verify, announce.
 * Returns LOOP_FINISHED, or LOOP_PAUSE / LOOP_STOP when interrupted while verifying. */
static loop_result_t complete_download(flux_download_t *dl) {
    uint64_t total = ALOAD(&dl->total_size);
    if (total == 0) {                                 /* size was unknown: it is whatever arrived */
        total = bytes_on_disk(dl);
        ASTORE(&dl->total_size, total);
    }

    if (dl->file_fd >= 0) {
        if (ftruncate(dl->file_fd, (off_t)total) < 0) {
            finish_fail(dl, FLUX_ERR_DISK_WRITE, "cannot finalise the output file", 1);
            return LOOP_FINISHED;
        }
        if (fdatasync(dl->file_fd) < 0 && (errno == EIO || errno == ENOSPC || errno == EDQUOT)) {
            finish_fail(dl, FLUX_ERR_DISK_WRITE, "writing to disk failed at the end of the download", 1);
            return LOOP_FINISHED;
        }
    }

    if (dl->cksum_type != FLUX_CKSUM_NONE) {
        uint8_t digest[FLUX_CKSUM_MAX_LEN];
        size_t  dlen = 0;

        for (;;) {
            int rc = hash_data_file(dl, total, digest, &dlen);
            if (rc == 0) break;
            if (rc < 0) {
                finish_fail(dl, flux_last_error(), flux_last_error_str(), 1);
                return LOOP_FINISHED;
            }
            if (ALOAD(&dl->stop_mode)) return LOOP_STOP;
            if (claim_pause(dl))       return LOOP_PAUSE;
            /* the pause request was withdrawn: verify again from the start */
        }

        size_t want = dl->cksum_type == FLUX_CKSUM_MD5 ? 16u : 32u;
        memcpy(dl->cksum_actual, digest, dlen > sizeof(dl->cksum_actual) ? sizeof(dl->cksum_actual) : dlen);
        dl->cksum_ok = (uint8_t)(dlen == want && memcmp(digest, dl->cksum_expected, want) == 0);

        if (!dl->cksum_ok) {
            char got_hex[2 * FLUX_CKSUM_MAX_LEN + 1], want_hex[2 * FLUX_CKSUM_MAX_LEN + 1], msg[200];
            flux_cksum_hex(digest, dlen, got_hex);
            flux_cksum_hex(dl->cksum_expected, want, want_hex);
            snprintf(msg, sizeof(msg), "checksum mismatch: expected %.16s..., got %.16s... (file removed)", want_hex, got_hex);

            /* A file that failed verification must not sit at its final name
             * looking finished, and its resume state must never be reused. */
            if (dl->file_fd >= 0) { close(dl->file_fd); dl->file_fd = -1; }
            unlink(dl->file_path);
            remove_resume_files(dl);
            finish_fail(dl, FLUX_ERR_CHECKSUM_, msg, 0);
            return LOOP_FINISHED;
        }
    }

    remove_resume_files(dl);
    dl->finished_ms = flux_now_ms();
    ASTORE(&dl->speed_bps, 0u);
    ASTORE(&dl->num_segs_active, 0);
    set_state(dl, DL_COMPLETE);
    flux_log(1, "download %u complete: %llu bytes in %.1f s", dl->id, (unsigned long long)total,
             (double)(dl->finished_ms - dl->started_ms) / 1000.0);
    emit(dl, FLUX_RSP_COMPLETE);
    return LOOP_FINISHED;
}

/* ═══ Run loop ════════════════════════════════════════════════════════════ */

static void update_speed(flux_download_t *dl, uint64_t now) {
    uint64_t dt = now - dl->speed_mark_ms;
    if (dt == 0) return;

    uint64_t cur   = ALOAD(&dl->downloaded_bytes);
    uint64_t delta = cur > dl->speed_mark_bytes ? cur - dl->speed_mark_bytes : 0u;   /* can dip after a restart */
    uint64_t inst  = delta * 1000u / dt;
    uint64_t prev  = ALOAD(&dl->speed_bps);
    uint64_t avg   = prev ? (prev * 3u + inst) / 4u : inst;                          /* smooths the display */

    ASTORE(&dl->speed_bps, (uint32_t)(avg > UINT32_MAX ? UINT32_MAX : avg));
    dl->speed_mark_bytes = cur;
    dl->speed_mark_ms    = now;
}

static void start_segments(flux_download_t *dl) {
    for (unsigned i = 0; i < dl->num_segs; i++) {
        flux_segment_t *seg = dl->segs[i];
        if (i == 0 && dl->probe_conn) {
            flux_conn_t     *conn = dl->probe_conn;
            http_response_t *resp = dl->probe_resp;
            dl->probe_conn = NULL;
            dl->probe_resp = NULL;
            flux_seg_adopt(seg, dl->epoll_fd, conn, resp);   /* owns conn from here on */
            free(resp);
        } else {
            flux_seg_start(seg, dl->epoll_fd);
        }
    }
}

static loop_result_t run_loop(flux_download_t *dl) {
    struct epoll_event evs[FLUX_EPOLL_MAX_EVENTS];

    uint64_t now = flux_now_ms();
    dl->speed_mark_ms       = now;
    dl->speed_mark_bytes    = ALOAD(&dl->downloaded_bytes);
    dl->last_progress_ms    = now;
    dl->last_resume_save_ms = now;

    for (;;) {
        if (ALOAD(&dl->stop_mode)) return LOOP_STOP;
        if (ALOAD(&dl->want_paused) && claim_pause(dl)) return LOOP_PAUSE;

        /* Sleep until the earliest thing that needs doing. */
        now = flux_now_ms();
        uint64_t next = flux_min_u64(dl->last_progress_ms + FLUX_PROGRESS_INTERVAL_MS,
                                     dl->last_resume_save_ms + FLUX_RESUME_SAVE_MS);
        for (unsigned i = 0; i < dl->num_segs; i++)
            next = flux_min_u64(next, flux_seg_next_deadline(dl->segs[i]));
        int timeout = next <= now ? 0 : (int)flux_min_u64(next - now, 1000u);

        int n = epoll_wait(dl->epoll_fd, evs, FLUX_EPOLL_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            flux_set_syserror("epoll_wait");
            finish_fail(dl, flux_last_error(), flux_last_error_str(), 1);
            return LOOP_FINISHED;
        }

        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == dl) { drain_wake(dl); continue; }
            flux_seg_on_event((flux_segment_t *)evs[i].data.ptr, evs[i].events);
        }

        now = flux_now_ms();
        unsigned done = 0, active = 0;
        for (unsigned i = 0; i < dl->num_segs; i++) {
            flux_segment_t *seg = dl->segs[i];
            flux_seg_tick(seg, now);

            if (seg->state == SEG_ERROR) {
                flux_error_t code = seg->err_code;
                char         msg[sizeof(seg->err_msg)];
                snprintf(msg, sizeof(msg), "%s", seg->err_msg);
                finish_fail(dl, code, msg, 1);
                return LOOP_FINISHED;
            }
            if (seg->state == SEG_COMPLETE)      done++;
            else if (seg->state != SEG_PENDING)  active++;
        }
        ASTORE(&dl->num_segs_active, (uint8_t)active);
        if (done == dl->num_segs) return LOOP_DONE;

        if (now - dl->speed_mark_ms >= FLUX_PROGRESS_INTERVAL_MS) {
            update_speed(dl, now);
            dl->last_progress_ms = now;
            emit(dl, FLUX_RSP_PROGRESS);
        }
        if (now - dl->last_resume_save_ms >= FLUX_RESUME_SAVE_MS) {
            if (checkpoint(dl) < 0) {
                finish_fail(dl, flux_last_error(), flux_last_error_str(), 1);
                return LOOP_FINISHED;
            }
        }
    }
}

/* ═══ Worker thread ═══════════════════════════════════════════════════════ */

static void dl_cleanup(flux_download_t *dl) {
    for (unsigned i = 0; i < FLUX_DL_MAX_SEGMENTS; i++) {
        flux_seg_free(dl->segs[i]);
        dl->segs[i] = NULL;
    }
    if (dl->probe_conn) { flux_conn_close(dl->probe_conn); dl->probe_conn = NULL; }
    free(dl->probe_resp);  dl->probe_resp = NULL;
    free(dl->resume);      dl->resume     = NULL;
    if (dl->file_fd  >= 0) { close(dl->file_fd);  dl->file_fd  = -1; }
    if (dl->epoll_fd >= 0) { close(dl->epoll_fd); dl->epoll_fd = -1; }
    ASTORE(&dl->num_segs_active, 0);
}

static void dl_run(flux_download_t *dl) {
    dl->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (dl->epoll_fd < 0) {
        flux_set_syserror("epoll_create1");
        finish_fail(dl, flux_last_error(), flux_last_error_str(), 0);
        return;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = dl };
    epoll_ctl(dl->epoll_fd, EPOLL_CTL_ADD, dl->wake_fd, &ev);

    /* ── probe (repeated if a pause request is withdrawn mid-way) ── */
    for (;;) {
        int pr = dl_probe(dl);
        if (pr == PROBE_OK) break;
        if (pr == PROBE_FAILED) {
            finish_fail(dl, flux_last_error(), flux_last_error_str(), 0);
            return;
        }
        if (ALOAD(&dl->stop_mode)) { finish_stop(dl);      return; }
        if (claim_pause(dl))       { finish_halt(dl, 1);   return; }
    }

    /* ── prepare ── */
    if (dl_prepare(dl) < 0) {
        finish_fail(dl, flux_last_error(), flux_last_error_str(), 0);
        return;
    }

    loop_result_t res;
    if (ALOAD(&dl->stop_mode)) {
        res = LOOP_STOP;
    } else if (ALOAD(&dl->want_paused) && claim_pause(dl)) {
        res = LOOP_PAUSE;
    } else if (dl->empty) {
        set_state(dl, DL_RUNNING);
        res = LOOP_DONE;
    } else {
        set_state(dl, DL_RUNNING);
        emit(dl, FLUX_RSP_PROGRESS);                  /* the client learns the size at once */
        start_segments(dl);
        res = run_loop(dl);
    }

    if (res == LOOP_DONE) res = complete_download(dl);
    if (res == LOOP_PAUSE)     finish_halt(dl, 1);
    else if (res == LOOP_STOP) finish_stop(dl);
}

static void *worker_main(void *arg) {
    flux_download_t *dl = arg;

    dl_run(dl);
    dl_cleanup(dl);

    /* Every path above set a final state; this only guards against a path that
     * forgot, so a client is never left waiting on a dead worker. */
    dl_state_t s = get_state(dl);
    if (s == DL_PROBING || s == DL_RUNNING || s == DL_IDLE)
        finish_fail(dl, FLUX_ERR_INTERNAL, "download stopped unexpectedly", 0);

    pthread_mutex_lock(&dl->lock);
    if (dl->wake_fd >= 0) { close(dl->wake_fd); dl->wake_fd = -1; }
    ASTORE(&dl->finished, 1);
    pthread_cond_broadcast(&dl->cv);
    pthread_mutex_unlock(&dl->lock);
    return NULL;
}

/* ═══ IPC-thread API ══════════════════════════════════════════════════════ */

/* The download cannot be started at all: report it the way a failed worker would. */
static void fail_launch(flux_download_t *dl, flux_error_t code, const char *msg) {
    dl->err_code = code;
    snprintf(dl->err_msg, sizeof(dl->err_msg), "%s", msg);
    dl->finished_ms = flux_now_ms();
    ASTORE(&dl->speed_bps, 0u);
    set_state(dl, DL_ERROR);
    ASTORE(&dl->finished, 1);
    flux_log(1, "download %u failed to start: %s", dl->id, dl->err_msg);
    emit(dl, FLUX_RSP_ERROR);
}

/* Start (or restart) the worker. The previous worker, if any, has been joined. */
static int launch_worker(flux_download_t *dl) {
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        flux_set_syserror("eventfd");
        fail_launch(dl, flux_last_error(), flux_last_error_str());
        return 0;
    }
    pthread_mutex_lock(&dl->lock);
    dl->wake_fd = fd;
    pthread_mutex_unlock(&dl->lock);

    dl->thread_started = 0;
    dl->joined         = 0;
    dl->finished_at_ms = 0;
    dl->addr_len       = 0;                           /* a stale peer address may be dead */
    dl->err_code       = FLUX_OK;
    dl->err_msg[0]     = '\0';
    dl->target         = dl->origin;                  /* start again from the URL as given */
    dl->use_tls        = (uint8_t)http_url_is_tls(&dl->origin);
    dl->started_ms     = flux_now_ms();
    ASTORE(&dl->want_paused, 0);
    ASTORE(&dl->stop_mode, DL_STOP_NONE);
    ASTORE(&dl->speed_bps, 0u);
    ASTORE(&dl->finished, 0);
    set_state(dl, DL_PROBING);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, WORKER_STACK);
    int rc = pthread_create(&dl->thread, &attr, worker_main, dl);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        errno = rc;
        flux_set_syserror("pthread_create");
        pthread_mutex_lock(&dl->lock);
        close(dl->wake_fd);
        dl->wake_fd = -1;
        pthread_mutex_unlock(&dl->lock);
        fail_launch(dl, flux_last_error(), flux_last_error_str());
        return 0;
    }
    dl->thread_started = 1;                           /* join is only ever valid after this */
    return 0;
}

flux_download_t *flux_dl_start(flux_registry_t   *reg,
                               uint32_t           id,
                               const char        *url,
                               const char        *dest_path,
                               uint8_t            num_segs,
                               uint64_t           bw_limit,
                               flux_cksum_type_t  cksum_type,
                               const uint8_t     *cksum_expected,
                               flux_event_cb      on_event) {
    if (!reg || !url || !dest_path) {
        flux_set_error(FLUX_ERR_BAD_STATE, "flux_dl_start: missing argument");
        return NULL;
    }
    if (id == 0) {
        flux_set_error(FLUX_ERR_BAD_STATE, "download id 0 is reserved for daemon-wide commands");
        return NULL;
    }
    if (url[0] == '\0' || strlen(url) >= FLUX_MAX_URL) {
        flux_set_error(FLUX_ERR_BAD_URL, "the URL is empty or too long");
        return NULL;
    }
    size_t plen = strlen(dest_path);
    if (plen == 0 || plen >= FLUX_MAX_PATH || dest_path[plen - 1] == '/') {
        flux_set_error(FLUX_ERR_DISK_OPEN, "invalid destination path");
        return NULL;
    }
    if (cksum_type != FLUX_CKSUM_NONE && cksum_type != FLUX_CKSUM_MD5 && cksum_type != FLUX_CKSUM_SHA256) {
        flux_set_error(FLUX_ERR_BAD_STATE, "unknown checksum type %d", (int)cksum_type);
        return NULL;
    }

    http_url_t origin;
    if (http_url_parse(url, &origin) < 0) {
        char why[sizeof(((flux_download_t *)0)->err_msg)];
        snprintf(why, sizeof(why), "%s", flux_last_error_str());   /* copy: the store is about to be overwritten */
        flux_set_error(FLUX_ERR_BAD_URL, "invalid URL: %s", why);
        return NULL;
    }

    flux_download_t *dl = dl_alloc();
    if (!dl) {
        flux_set_error(FLUX_ERR_NOMEM, "cannot allocate a download");
        return NULL;
    }

    dl->id = id;
    snprintf(dl->url, sizeof(dl->url), "%s", url);
    snprintf(dl->file_path, sizeof(dl->file_path), "%s", dest_path);
    flux_resume_path(dl->file_path, dl->part_path, sizeof(dl->part_path));
    dl->want_segs     = num_segs == 0 ? DEFAULT_SEGMENTS
                      : num_segs > FLUX_DL_MAX_SEGMENTS ? FLUX_DL_MAX_SEGMENTS : num_segs;
    dl->cksum_type    = (uint8_t)cksum_type;
    if (cksum_expected) memcpy(dl->cksum_expected, cksum_expected, FLUX_CKSUM_MAX_LEN);
    dl->origin        = origin;
    dl->target        = origin;
    dl->use_tls       = (uint8_t)http_url_is_tls(&origin);
    dl->global_bucket = &reg->global_bw;
    dl->on_event      = on_event;
    flux_bw_set_rate(&dl->bw_bucket, bw_limit);
    set_state(dl, DL_IDLE);

    pthread_mutex_lock(&reg->lock);

    /* The same id again: refuse while its worker is running, replace it
     * otherwise (a finished, failed or paused download being started afresh —
     * its partial file is picked up through the resume state). */
    flux_download_t *old = reg_find_locked(reg, id);
    if (old) {
        if (dl_alive(old)) {
            pthread_mutex_unlock(&reg->lock);
            flux_set_error(FLUX_ERR_DUPLICATE, "download %u is already running", id);
            dl_free(dl);
            return NULL;
        }
        dl_retire_locked(reg, old);
    }

    /* Two live downloads writing the same file would corrupt each other. */
    for (unsigned b = 0; b < FLUX_REG_BUCKETS; b++) {
        for (flux_download_t *d = reg->buckets[b]; d; d = d->hash_next) {
            if (strcmp(d->file_path, dest_path) == 0 && (dl_alive(d) || get_state(d) == DL_PAUSED)) {
                uint32_t other = d->id;
                pthread_mutex_unlock(&reg->lock);
                flux_set_error(FLUX_ERR_DUPLICATE, "the destination file is already used by download %u", other);
                dl_free(dl);
                return NULL;
            }
        }
    }

    reg_insert_locked(reg, dl);
    pthread_mutex_unlock(&reg->lock);
    return dl;
}

int flux_dl_launch(flux_download_t *dl) {
    if (!dl) return -1;
    return launch_worker(dl);
}

int flux_dl_pause(flux_download_t *dl) {
    dl_state_t s = get_state(dl);
    if (s == DL_PAUSED) return 0;                                  /* already there */
    if (s != DL_PROBING && s != DL_RUNNING) {
        flux_set_error(FLUX_ERR_BAD_STATE, "download %u is %s and cannot be paused", dl->id, state_name(s));
        return -1;
    }

    int expected = 0;
    __atomic_compare_exchange_n(&dl->want_paused, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    wake_worker(dl);

    /* Give the worker a moment to stop so the reply tells the truth. A worker
     * stuck in a DNS lookup cannot react until the lookup returns: the request
     * stays pending and takes effect then. */
    if (wait_finished(dl, PAUSE_WAIT_MS) && get_state(dl) != DL_PAUSED) {
        flux_set_error(FLUX_ERR_BAD_STATE, "download %u finished (%s) before it could be paused",
                       dl->id, state_name(get_state(dl)));
        return -1;
    }
    return 0;
}

int flux_dl_resume(flux_download_t *dl) {
    if (dl_alive(dl)) {
        int expected = 1;
        if (__atomic_compare_exchange_n(&dl->want_paused, &expected, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            wake_worker(dl);                                       /* pause withdrawn before it took effect */
            return 0;
        }
        if (ALOAD(&dl->want_paused) != 2) return 0;                /* running normally: nothing to do */

        /* The worker is committed to pausing: let it finish, then relaunch. */
        if (!wait_finished(dl, PAUSE_WAIT_MS)) {
            flux_set_error(FLUX_ERR_BAD_STATE, "download %u is still pausing; try again in a moment", dl->id);
            return -1;
        }
    }

    dl_state_t s = get_state(dl);
    if (s != DL_PAUSED && s != DL_ERROR) {
        flux_set_error(FLUX_ERR_BAD_STATE, "download %u is %s and cannot be resumed", dl->id, state_name(s));
        return -1;
    }
    if (dl->thread_started && !dl->joined) { pthread_join(dl->thread, NULL); dl->joined = 1; }
    return launch_worker(dl);
}

int flux_dl_cancel(flux_registry_t *reg, flux_download_t *dl) {
    if (!dl) return 0;

    if (dl_alive(dl)) {
        ASTORE(&dl->stop_mode, DL_STOP_DELETE);
        wake_worker(dl);
        if (!wait_finished(dl, STOP_WAIT_MS)) {
            /* Blocked in a DNS lookup. It will see the request the moment the
             * lookup returns, delete its files and exit; the registry reaper
             * releases it then. */
            flux_log(1, "download %u: worker is slow to stop; it will be released when it exits", dl->id);
            return 0;
        }
    }

    /* The worker is gone. A cancelled one cleaned up after itself; a paused or
     * failed one left its partial file and resume state behind. A finished
     * download's output file is the user's now and is never touched. */
    dl_state_t s = get_state(dl);
    if (s != DL_COMPLETE && s != DL_CANCELLED) {
        if (dl->owns_file) unlink(dl->file_path);
        remove_resume_files(dl);
    }

    pthread_mutex_lock(&reg->lock);
    dl_retire_locked(reg, dl);
    pthread_mutex_unlock(&reg->lock);
    return 0;
}
