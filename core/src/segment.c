/* FluxDM — core/src/segment.c
 *
 * One segment = one byte range = one connection, driven by its download's
 * epoll loop. The state machine:
 *
 *   PENDING ─▶ CONNECTING ─▶ TLS_SHAKE ─▶ REQUESTING ─▶ HEADERS ─▶ BODY ─▶ COMPLETE
 *      ▲            │             │            │            │        │  ▲
 *      │            └─────────────┴────────────┴────────────┴────────┘  │
 *      │                         failure (retryable)                     │
 *      │                                 ▼                       THROTTLED
 *      └──── (halt: pause/stop) ──── RETRYING ──▶ (back-off) ──▶ CONNECTING
 *                                     failure (fatal / out of retries) ─▶ ERROR
 *
 * Nothing here blocks: every call does what it can and returns, and flux_seg_tick()
 * plus the epoll events pick the work up again.
 *
 * Two kinds of "try again later" share SEG_RETRYING:
 *   - a connection failed (network error, 5xx): seg_fail() backs off and spends
 *     one of the segment's retries;
 *   - the server pushed back (429 / 503), or the host's allowance of connections
 *     is used up: the segment waits for a slot without spending retries, within
 *     a time budget (seg_refused(), hostgate.h).
 */

#include "../include/segment.h"
#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/util.h"
#include "../include/protocol/http.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <stdio.h>

#define SEG_MAX_RETRIES         8
#define SEG_BACKOFF_BASE_MS     500u
#define SEG_BACKOFF_MAX_MS      15000u
#define SEG_BACKOFF             UINT32_MAX     /* "use the computed back-off" */
#define SEG_PUMP_BUDGET         (1u << 20)     /* bytes per pump call, then yield */
#define SEG_FLUSH_IDLE_MS       1000u          /* flush a partially filled buffer after this */
#define SEG_RETRY_RESET_BYTES   (16u * 1024u)  /* progress that forgives earlier failures */

/* ── Small helpers ────────────────────────────────────────────────────────── */

/* True if the I/O call that just failed merely had nothing to do. Valid only
 * immediately after a failing flux_conn_* call, which always sets the error. */
static int would_block(void) { return flux_last_error() == FLUX_ERR_WOULDBLOCK; }

static void seg_epoll(flux_segment_t *seg, uint32_t events) {
    if (!seg->conn || seg->conn->fd < 0 || seg->epoll_fd < 0) return;
    if (events == seg->epoll_events) return;

    struct epoll_event ev = { .events = events, .data.ptr = seg };
    int op = events == 0                ? EPOLL_CTL_DEL
           : seg->epoll_events == 0     ? EPOLL_CTL_ADD
                                        : EPOLL_CTL_MOD;
    epoll_ctl(seg->epoll_fd, op, seg->conn->fd, &ev);
    seg->epoll_events = events;
}

static void seg_close_conn(flux_segment_t *seg) {
    if (seg->conn) {
        if (seg->epoll_events && seg->conn->fd >= 0 && seg->epoll_fd >= 0)
            epoll_ctl(seg->epoll_fd, EPOLL_CTL_DEL, seg->conn->fd, NULL);
        flux_conn_close(seg->conn);
        seg->conn = NULL;
    }
    seg->epoll_events = 0;
    flux_gate_release(seg->gate);          /* the slot goes back whenever the connection does */
    seg->gate = NULL;
}

/* The server is serving this segment again: forget its earlier refusals. */
static void seg_clear_refusals(flux_segment_t *seg) {
    seg->refused_since_ms = 0;
    seg->refusals         = 0;
}

static void seg_release_wbuf(flux_segment_t *seg) {
    if (seg->wbuf_len == 0) { free(seg->wbuf); seg->wbuf = NULL; }
}

/* ── Accounting helpers (public) ──────────────────────────────────────────── */

uint64_t flux_seg_received(const flux_segment_t *seg) {
    return seg->written + seg->wbuf_len;
}

uint64_t flux_seg_remaining(const flux_segment_t *seg) {
    if (seg->range_end == UINT64_MAX) return UINT64_MAX;
    uint64_t total = seg->range_end - seg->range_start + 1u;
    uint64_t got   = flux_seg_received(seg);
    return total > got ? total - got : 0u;
}

uint64_t flux_seg_offset(const flux_segment_t *seg) {
    return seg->range_start + flux_seg_received(seg);
}

void flux_seg_update_speed(flux_segment_t *seg, uint64_t now, size_t bytes) {
    seg->speed_sample_bytes += bytes;
    uint64_t elapsed = now - seg->speed_sample_ms;
    if (elapsed >= 500) {
        seg->speed_bps          = (uint32_t)(seg->speed_sample_bytes * 1000u / elapsed);
        seg->speed_sample_bytes = 0;
        seg->speed_sample_ms    = now;
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

flux_segment_t *flux_seg_new(struct flux_download *dl, uint8_t index,
                              uint64_t start, uint64_t end) {
    flux_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg) {
        flux_set_error(FLUX_ERR_NOMEM, "flux_seg_new: calloc");
        return NULL;
    }
    seg->response = calloc(1, sizeof(http_response_t));
    if (!seg->response) {
        free(seg);
        flux_set_error(FLUX_ERR_NOMEM, "flux_seg_new: response calloc");
        return NULL;
    }

    seg->index        = index;
    seg->state        = SEG_PENDING;
    seg->range_start  = start;
    seg->range_end    = end;
    seg->dl           = dl;
    seg->file_fd      = dl->file_fd;
    seg->epoll_fd     = -1;
    seg->max_retries  = SEG_MAX_RETRIES;
    return seg;
}

void flux_seg_abort(flux_segment_t *seg) {
    seg_close_conn(seg);
    /* Discarded bytes were already counted as downloaded; take them back. */
    if (seg->wbuf_len > 0) flux_dl_add_progress(seg->dl, -(int64_t)seg->wbuf_len);
    seg->wbuf_len = 0;
    seg_release_wbuf(seg);
    seg->state = SEG_PENDING;
}

void flux_seg_free(flux_segment_t *seg) {
    if (!seg) return;
    seg_close_conn(seg);
    free(seg->wbuf);
    free(seg->response);
    free(seg);
}

/* ── Disk ─────────────────────────────────────────────────────────────────── */

int flux_seg_flush(flux_segment_t *seg) {
    if (seg->wbuf_len == 0) return 0;

    uint64_t offset = seg->range_start + seg->written;
    size_t   done   = 0;
    while (done < seg->wbuf_len) {
        ssize_t n = pwrite(seg->file_fd, seg->wbuf + done, seg->wbuf_len - done,
                           (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            flux_set_error((e == ENOSPC || e == EDQUOT) ? FLUX_ERR_DISK_SPACE : FLUX_ERR_DISK_WRITE,
                           "write at offset %llu failed: %s",
                           (unsigned long long)(offset + done), strerror(e));
            return -1;
        }
        if (n == 0) {
            flux_set_error(FLUX_ERR_DISK_WRITE, "write at offset %llu made no progress",
                           (unsigned long long)(offset + done));
            return -1;
        }
        done += (size_t)n;
    }
    seg->written      += seg->wbuf_len;
    seg->wbuf_len      = 0;
    seg->last_flush_ms = flux_now_ms();
    return 0;
}

/* ── Failure handling ─────────────────────────────────────────────────────── */

static uint32_t backoff_ms(const flux_segment_t *seg) {
    /* retries has already been incremented: 1st retry waits BASE, then doubles */
    unsigned shift = seg->retries > 1 ? (unsigned)(seg->retries - 1) : 0u;
    if (shift > 6) shift = 6;
    uint32_t ms = SEG_BACKOFF_BASE_MS << shift;
    if (ms > SEG_BACKOFF_MAX_MS) ms = SEG_BACKOFF_MAX_MS;
    /* ±12% deterministic jitter so sibling segments don't reconnect in lockstep */
    uint32_t h = (uint32_t)seg->index * 2654435761u + (uint32_t)seg->retries * 40503u;
    int32_t  j = (int32_t)(h % 25u) - 12;
    return (uint32_t)((int64_t)ms + (int64_t)ms * j / 100);
}

/* The start of every failure: remember the cause (it must already be in the
 * thread-local error store), keep the bytes we have — a retry resumes right after
 * them — and drop the connection. Returns 1 when flushing hit a disk error,
 * which outranks whatever went wrong on the network and ends the segment. */
static int seg_abandon_attempt(flux_segment_t *seg) {
    int disk_error = 0;

    seg->err_code = flux_last_error();
    snprintf(seg->err_msg, sizeof(seg->err_msg), "%s", flux_last_error_str());

    if (seg->wbuf_len > 0 && flux_seg_flush(seg) < 0) {
        seg->err_code = flux_last_error();
        snprintf(seg->err_msg, sizeof(seg->err_msg), "%s", flux_last_error_str());
        disk_error = 1;
    }
    seg_close_conn(seg);
    seg->deadline_ms = 0;
    seg->pump_again  = 0;
    return disk_error;
}

/* Record a failure. The cause must already be in the thread-local error store
 * (flux_set_error). `fatal` ends the segment; otherwise it backs off and is
 * restarted by flux_seg_tick() — from the byte after the last one received.
 * `delay_ms` overrides the computed back-off (SEG_BACKOFF = compute). */
static void seg_fail(flux_segment_t *seg, int fatal, uint32_t delay_ms) {
    uint64_t now = flux_now_ms();

    if (seg_abandon_attempt(seg)) fatal = 1;

    if (!fatal) {
        uint64_t got = flux_seg_received(seg);
        if (got >= seg->progress_mark + SEG_RETRY_RESET_BYTES) {   /* made real progress */
            seg->retries = 0;
            seg_clear_refusals(seg);
        }
        seg->progress_mark = got;

        if (seg->retries >= seg->max_retries) {
            fatal = 1;
            size_t n = strlen(seg->err_msg);
            snprintf(seg->err_msg + n, sizeof(seg->err_msg) - n,
                     " (gave up after %u retries)", (unsigned)seg->retries);
        }
    }

    if (fatal) {
        seg->state = SEG_ERROR;
        flux_log(1, "download %u segment %u failed: %s", seg->dl->id, seg->index, seg->err_msg);
        return;
    }

    seg->retries++;
    uint32_t delay = delay_ms == SEG_BACKOFF ? backoff_ms(seg) : delay_ms;
    seg->retry_delay_ms = delay;
    seg->retry_at_ms    = now + delay;
    seg->state          = SEG_RETRYING;
    flux_log(2, "download %u segment %u: %s — retry %u in %u ms", seg->dl->id, seg->index,
             seg->err_msg, (unsigned)seg->retries, (unsigned)delay);
}

/* The server answered 429 / 503: busy or rationing, not broken. That is no fault
 * of the connection, so it does not spend the retry budget; the segment backs off
 * (at least as long as the server asked), waits for a slot and asks again — for
 * as long as the patience lasts, so a rate limit that clears in a minute does
 * not kill a download. The gate hears of it too: the host's other segments, and
 * other downloads to it, ease off instead of all being refused in step.
 * The cause (the HTTP status) must already be in the thread-local error store. */
static void seg_refused(flux_segment_t *seg, uint32_t retry_after_ms) {
    uint64_t now     = flux_now_ms();
    uint32_t lowered = flux_gate_throttled(seg->gate, now, retry_after_ms);   /* needs the slot: before it goes back */
    int      fatal   = seg_abandon_attempt(seg);

    if (lowered)
        flux_log(1, "%s:%u is refusing connections (%s) — using at most %u at a time for now",
                 seg->dl->target.host, (unsigned)seg->dl->target.port, seg->err_msg, (unsigned)lowered);

    uint32_t patience = flux_gate_patience_ms();
    uint64_t waited   = 0;
    uint32_t delay    = 0;

    if (!fatal) {
        uint64_t got = flux_seg_received(seg);
        if (got >= seg->progress_mark + SEG_RETRY_RESET_BYTES) {          /* made real progress since */
            seg->retries = 0;
            seg_clear_refusals(seg);
        }
        seg->progress_mark = got;

        if (seg->refused_since_ms == 0) seg->refused_since_ms = now;
        waited = now - seg->refused_since_ms;

        size_t n = strlen(seg->err_msg);
        if (waited >= patience) {
            fatal = 1;
            snprintf(seg->err_msg + n, sizeof(seg->err_msg) - n,
                     " (the server kept refusing for %u s)", (unsigned)(waited / 1000u));
        } else if (retry_after_ms > patience - waited) {
            fatal = 1;
            snprintf(seg->err_msg + n, sizeof(seg->err_msg) - n,
                     " (the server asks us to wait %u s)", (unsigned)(retry_after_ms / 1000u));
        } else {
            delay = flux_gate_backoff_ms(seg->refusals, seg->index);
            if (retry_after_ms > delay) delay = retry_after_ms;
            if (delay > patience - waited) delay = (uint32_t)(patience - waited);
        }
    }

    if (fatal) {
        seg->state = SEG_ERROR;
        flux_log(1, "download %u segment %u failed: %s", seg->dl->id, seg->index, seg->err_msg);
        return;
    }

    if (seg->refusals < UINT8_MAX) seg->refusals++;
    seg->retry_delay_ms = delay;
    seg->retry_at_ms    = now + delay;
    seg->state          = SEG_RETRYING;
    flux_log(2, "download %u segment %u: %s — asking again in %u ms", seg->dl->id, seg->index,
             seg->err_msg, (unsigned)delay);
}

/* ── Completion ───────────────────────────────────────────────────────────── */

static void seg_finish(flux_segment_t *seg) {
    if (seg->wbuf_len > 0 && flux_seg_flush(seg) < 0) { seg_fail(seg, 1, 0); return; }
    seg_close_conn(seg);
    free(seg->wbuf);
    seg->wbuf         = NULL;
    seg->state        = SEG_COMPLETE;
    seg->deadline_ms  = 0;
    seg->pump_again   = 0;
    seg->retries      = 0;
    seg_clear_refusals(seg);
}

/* ── Starting ─────────────────────────────────────────────────────────────── */

/* A server that cannot serve ranges can only ever stream from byte 0. If such
 * a stream breaks, everything received so far is useless as a prefix of the
 * next attempt — it gets overwritten — so forget it. */
static void seg_restart_from_zero(flux_segment_t *seg) {
    int64_t got = (int64_t)flux_seg_received(seg);
    if (got > 0) flux_dl_add_progress(seg->dl, -got);
    seg->written  = 0;
    seg->wbuf_len = 0;
    seg->progress_mark = 0;
}

int flux_seg_start(flux_segment_t *seg, int epoll_fd) {
    flux_download_t *dl  = seg->dl;
    uint64_t         now = flux_now_ms();

    seg->epoll_fd = epoll_fd;
    if (seg->started_at_ms == 0) seg->started_at_ms = now;
    seg->last_active_ms  = now;
    seg->speed_sample_ms = now;
    seg->last_flush_ms   = now;     /* the idle-flush clock starts with the attempt */

    seg_close_conn(seg);

    if (flux_seg_remaining(seg) == 0) { seg->state = SEG_COMPLETE; return 0; }

    if (!dl->accept_ranges && flux_seg_received(seg) > 0) seg_restart_from_zero(seg);

    /* A slot for the connection — or wait for one. Waiting is not a failure and
     * spends no retry: the host simply cannot take another connection yet (it has
     * said no, or asked for a pause), and the gate says when to look again. */
    uint32_t wait_ms = 0;
    if (!flux_gate_acquire(dl->target.host, dl->target.port, now, &seg->gate, &wait_ms)) {
        seg->retry_delay_ms = wait_ms;
        seg->retry_at_ms    = now + wait_ms;
        seg->state          = SEG_RETRYING;
        return 0;
    }

    if (!seg->wbuf) {
        seg->wbuf = malloc(FLUX_WRITE_BUF_CAP);
        if (!seg->wbuf) {
            flux_set_error(FLUX_ERR_NOMEM, "segment %u: cannot allocate write buffer", seg->index);
            seg_fail(seg, 1, 0);
            return -1;
        }
    }

    seg->conn = flux_conn_new(dl->target.host, dl->target.port, dl->use_tls);
    if (!seg->conn) { seg_fail(seg, 1, 0); return -1; }
    if (dl->addr_len > 0)
        flux_conn_set_addr(seg->conn, (const struct sockaddr *)&dl->addr, dl->addr_len);

    if (flux_conn_start_connect(seg->conn) < 0) {
        seg_fail(seg, 0, SEG_BACKOFF);
        return seg->state == SEG_ERROR ? -1 : 0;
    }

    seg->state       = SEG_CONNECTING;
    seg->deadline_ms = now + FLUX_CONN_CONNECT_TOMS;
    seg_epoll(seg, EPOLLOUT);
    return 0;
}

/* ── Request ──────────────────────────────────────────────────────────────── */

static void seg_send_request(flux_segment_t *seg) {
    while (seg->req_sent < seg->req_len) {
        ssize_t n = flux_conn_send(seg->conn, seg->req + seg->req_sent,
                                   seg->req_len - seg->req_sent);
        if (n < 0) {
            if (would_block()) { seg_epoll(seg, EPOLLOUT); return; }
            seg_fail(seg, 0, SEG_BACKOFF);
            return;
        }
        seg->req_sent += (size_t)n;
    }

    seg->state       = SEG_HEADERS;
    seg->deadline_ms = flux_now_ms() + FLUX_CONN_RECV_TOMS;
    seg_epoll(seg, EPOLLIN);
}

static void seg_begin_request(flux_segment_t *seg) {
    flux_download_t *dl = seg->dl;

    seg->req_offset = flux_seg_offset(seg);
    seg->req_gen    = dl->target_gen;

    /* A plain GET unless this request covers less than the whole file: it keeps
     * single-stream downloads working on servers with flaky Range handling. */
    int whole_file = seg->req_offset == 0 && seg->range_start == 0 &&
                     (seg->range_end == UINT64_MAX ||
                      (dl->total_size && seg->range_end + 1u == dl->total_size));
    seg->req_ranged = dl->accept_ranges && !whole_file;

    int n = http_build_get(seg->req, sizeof(seg->req), &dl->target,
                           seg->req_ranged, seg->req_offset, seg->range_end);
    if (n < 0) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "request does not fit its buffer (URL too long)");
        seg_fail(seg, 1, 0);
        return;
    }
    seg->req_len  = (size_t)n;
    seg->req_sent = 0;
    seg->state    = SEG_REQUESTING;
    seg_send_request(seg);
}

/* ── Connect / TLS ────────────────────────────────────────────────────────── */

static void seg_on_connecting(flux_segment_t *seg) {
    int rc = (seg->state == SEG_CONNECTING) ? flux_conn_finish_connect(seg->conn)
                                            : flux_conn_tls_handshake_continue(seg->conn);
    if (rc < 0) {
        /* A bad certificate will be just as bad next time: don't retry. */
        seg_fail(seg, flux_last_error() == FLUX_ERR_TLS_CERT, SEG_BACKOFF);
        return;
    }
    if (rc == 0 || rc == 2) {                 /* TLS still negotiating; wait for the direction it asked for */
        seg->state = SEG_TLS_SHAKE;
        seg_epoll(seg, rc == 2 ? EPOLLOUT : EPOLLIN);
        return;
    }
    seg_begin_request(seg);
}

/* ── Chunked decoding ─────────────────────────────────────────────────────── */

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode buffered chunked framing into wbuf. At most `want` payload bytes are
 * appended (the caller bumps wbuf_len). Returns the number appended, or -1 on
 * malformed framing. *need_more is set when the buffer ran dry mid-frame. */
static ssize_t seg_chunk_decode(flux_segment_t *seg, size_t want, int *need_more) {
    flux_conn_t *c   = seg->conn;
    size_t       out = 0;
    *need_more = 0;

    for (;;) {
        size_t         avail = flux_conn_buffered(c);
        const uint8_t *p     = c->rbuf + c->rbuf_start;

        switch ((seg_chunk_state_t)seg->chunk_state) {

        case CH_SIZE: {
            const uint8_t *nl = memchr(p, '\n', avail < 80 ? avail : 80);
            if (!nl) {
                if (avail >= 80) { flux_set_error(FLUX_ERR_HTTP_PARSE, "chunk-size line too long"); return -1; }
                *need_more = 1;
                return (ssize_t)out;
            }
            size_t   line = (size_t)(nl - p) + 1;
            uint64_t sz   = 0;
            size_t   i    = 0;
            for (; i < line; i++) {
                int h = hex_digit(p[i]);
                if (h < 0) break;
                if (sz > (UINT64_MAX >> 4)) { flux_set_error(FLUX_ERR_HTTP_PARSE, "chunk size overflow"); return -1; }
                sz = (sz << 4) | (uint64_t)h;
            }
            if (i == 0) { flux_set_error(FLUX_ERR_HTTP_PARSE, "malformed chunk-size line"); return -1; }
            flux_conn_drain(c, NULL, line);                 /* any ";extension" is ignored */
            if (sz == 0) {
                seg->chunk_state = CH_TRAILER;
            } else {
                seg->chunk_left  = sz;
                seg->chunk_state = CH_DATA;
            }
            break;
        }

        case CH_DATA: {
            if (out >= want) return (ssize_t)out;
            if (avail == 0) { *need_more = 1; return (ssize_t)out; }
            size_t take = avail;
            if (take > want - out)        take = want - out;
            if (take > seg->chunk_left)   take = (size_t)seg->chunk_left;
            flux_conn_drain(c, seg->wbuf + seg->wbuf_len + out, take);
            out             += take;
            seg->chunk_left -= take;
            if (seg->chunk_left == 0) seg->chunk_state = CH_CRLF;
            break;
        }

        case CH_CRLF:
            if (avail == 0) { *need_more = 1; return (ssize_t)out; }
            if (p[0] == '\n') {
                flux_conn_drain(c, NULL, 1);
            } else if (p[0] == '\r') {
                if (avail < 2) { *need_more = 1; return (ssize_t)out; }
                if (p[1] != '\n') { flux_set_error(FLUX_ERR_HTTP_PARSE, "bad chunk terminator"); return -1; }
                flux_conn_drain(c, NULL, 2);
            } else {
                flux_set_error(FLUX_ERR_HTTP_PARSE, "bad chunk terminator");
                return -1;
            }
            seg->chunk_state = CH_SIZE;
            break;

        case CH_TRAILER: {
            const uint8_t *nl = memchr(p, '\n', avail < 8192 ? avail : 8192);
            if (!nl) {
                if (avail >= 8192) { flux_set_error(FLUX_ERR_HTTP_PARSE, "chunk trailer too long"); return -1; }
                *need_more = 1;
                return (ssize_t)out;
            }
            size_t line  = (size_t)(nl - p) + 1;
            size_t blank = (line == 1 || (line == 2 && p[0] == '\r'));
            flux_conn_drain(c, NULL, line);
            if (blank) seg->chunk_state = CH_DONE;
            break;
        }

        case CH_DONE:
            return (ssize_t)out;
        }
    }
}

/* ── Body ─────────────────────────────────────────────────────────────────── */

/* The body ended (EOF, Content-Length reached, last chunk) before the segment
 * had everything it needs. Fine for an open-ended segment, a failure otherwise. */
static void seg_body_ended(flux_segment_t *seg) {
    if (seg->range_end == UINT64_MAX) { seg_finish(seg); return; }
    flux_set_error(FLUX_ERR_NETWORK, "server ended the response after %llu of %llu bytes",
                   (unsigned long long)flux_seg_received(seg),
                   (unsigned long long)(seg->range_end - seg->range_start + 1u));
    seg_fail(seg, 0, SEG_BACKOFF);
}

static void seg_pump(flux_segment_t *seg) {
    flux_download_t *dl     = seg->dl;
    size_t           budget = SEG_PUMP_BUDGET;

    while (seg->state == SEG_BODY) {
        uint64_t need = flux_seg_remaining(seg);
        if (need == 0) { seg_finish(seg); return; }

        if ((!seg->chunked && seg->body_left == 0) ||
            ( seg->chunked && seg->chunk_state == CH_DONE)) {
            seg_body_ended(seg);
            return;
        }

        if (budget == 0) { seg->pump_again = 1; return; }   /* let sibling segments run */

        size_t space = FLUX_WRITE_BUF_CAP - seg->wbuf_len;
        if (space == 0) {
            if (flux_seg_flush(seg) < 0) { seg_fail(seg, 1, 0); return; }
            continue;
        }

        size_t want = space;
        if (need < want)                                  want = (size_t)need;
        if (!seg->chunked && seg->body_left < want)       want = (size_t)seg->body_left;
        size_t cap = flux_dl_read_cap(dl);
        if (cap < want)                                   want = cap;
        if (budget < want)                                want = budget;

        ssize_t got;
        if (seg->chunked) {
            int need_more = 0;
            got = seg_chunk_decode(seg, want, &need_more);
            if (got < 0) { seg_fail(seg, 0, SEG_BACKOFF); return; }
            if (got == 0) {
                if (!need_more) continue;                   /* state advanced (CH_DONE etc.) */
                ssize_t n = flux_conn_fill(seg->conn);
                if (n > 0) continue;
                if (n == 0) {
                    if (seg->chunk_state == CH_DONE) continue;
                    flux_set_error(FLUX_ERR_NETWORK, "connection closed in the middle of a chunked body");
                    seg_fail(seg, 0, SEG_BACKOFF);
                    return;
                }
                if (would_block()) return;
                seg_fail(seg, 0, SEG_BACKOFF);
                return;
            }
        } else if (flux_conn_buffered(seg->conn) > 0) {
            got = (ssize_t)flux_conn_drain(seg->conn, seg->wbuf + seg->wbuf_len, want);
        } else {
            got = flux_conn_recv(seg->conn, seg->wbuf + seg->wbuf_len, want);
            if (got == 0) {                                   /* peer closed */
                if (seg->body_left == UINT64_MAX) { seg_body_ended(seg); return; }
                flux_set_error(FLUX_ERR_NETWORK, "connection closed with %llu body bytes missing",
                               (unsigned long long)seg->body_left);
                seg_fail(seg, 0, SEG_BACKOFF);
                return;
            }
            if (got < 0) {
                if (would_block()) return;                    /* wait for the next EPOLLIN */
                seg_fail(seg, 0, SEG_BACKOFF);
                return;
            }
        }

        /* got > 0: account for it */
        uint64_t now = flux_now_ms();
        seg->wbuf_len += (size_t)got;
        if (!seg->chunked && seg->body_left != UINT64_MAX) seg->body_left -= (uint64_t)got;
        budget = budget > (size_t)got ? budget - (size_t)got : 0;

        seg->last_active_ms = now;
        seg->deadline_ms    = now + FLUX_CONN_RECV_TOMS;
        flux_seg_update_speed(seg, now, (size_t)got);
        flux_dl_add_progress(dl, (int64_t)got);

        uint32_t wait = flux_dl_charge(dl, (size_t)got);
        if (wait > 0) {
            seg->throttle_until_ms = now + wait;
            seg->state             = SEG_THROTTLED;
            seg->deadline_ms       = 0;
            seg_epoll(seg, 0);                    /* stop watching: a HUP would spin otherwise */
            return;
        }
    }
}

/* ── Response head handling ───────────────────────────────────────────────── */

static void seg_headers_complete(flux_segment_t *seg) {
    flux_download_t *dl = seg->dl;
    http_response_t *r  = seg->response;
    int              st = r->status;

    flux_log(2, "download %u segment %u: HTTP %d (offset %llu, %s)", dl->id, seg->index, st,
             (unsigned long long)seg->req_offset, seg->req_ranged ? "ranged" : "plain");

    if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
        /* The first segment to see a redirect moves the whole download to the new
         * target; siblings that were already in flight to the old one just retry. */
        int stale = seg->req_gen != dl->target_gen;
        if (!stale && (r->location[0] == '\0' || flux_dl_note_redirect(dl, r->location) < 0)) {
            if (r->location[0] == '\0')
                flux_set_error(FLUX_ERR_HTTP_REDIR, "HTTP %d without a Location header", st);
            seg_fail(seg, 1, 0);
        } else {
            flux_set_error(FLUX_ERR_HTTP_REDIR, "redirected (HTTP %d)", st);
            seg_fail(seg, 0, 0);                  /* straight away, to the new target */
        }
        return;
    }

    if (http_status_is_pushback(st)) {            /* "slow down": see seg_refused() */
        flux_set_error(http_status_to_flux_error(st), "HTTP %d %s", st, r->reason);
        seg_refused(seg, r->retry_after_s * 1000u);
        return;
    }

    if (st >= 400 || st < 200) {
        flux_set_error(http_status_to_flux_error(st), "HTTP %d %s", st, r->reason);
        if (http_status_is_retryable(st)) {
            uint32_t d = backoff_ms(seg);
            if (r->retry_after_s * 1000u > d) d = r->retry_after_s * 1000u;
            seg_fail(seg, 0, d);
        } else {
            seg_fail(seg, 1, 0);
        }
        return;
    }

    if (st != 200 && st != 206) {
        flux_set_error(FLUX_ERR_HTTP_STATUS, "unexpected HTTP %d %s", st, r->reason);
        seg_fail(seg, 1, 0);
        return;
    }

    if (st == 206) {
        if (!seg->req_ranged) {
            flux_set_error(FLUX_ERR_HTTP_RANGE, "server answered a plain request with 206");
            seg_fail(seg, 1, 0);
            return;
        }
        if (!r->has_content_range || r->content_range_unsat ||
            r->content_range_start != seg->req_offset) {
            flux_set_error(FLUX_ERR_HTTP_RANGE, "server returned the wrong byte range (wanted from %llu)",
                           (unsigned long long)seg->req_offset);
            seg_fail(seg, 1, 0);
            return;
        }
        if (dl->total_size && r->content_range_total && r->content_range_total != dl->total_size) {
            flux_set_error(FLUX_ERR_REMOTE_CHANGED, "file size changed on the server (%llu -> %llu)",
                           (unsigned long long)dl->total_size, (unsigned long long)r->content_range_total);
            seg_fail(seg, 1, 0);
            return;
        }
        seg->body_left = r->content_range_end - r->content_range_start + 1u;
    } else {                                      /* 200 */
        if (seg->req_ranged) {
            flux_set_error(FLUX_ERR_HTTP_RANGE, "server ignored the byte-range request (HTTP 200 instead of 206)");
            seg_fail(seg, 1, 0);
            return;
        }
        if (r->has_content_length && dl->total_size && r->content_length != dl->total_size) {
            flux_set_error(FLUX_ERR_REMOTE_CHANGED, "file size changed on the server (%llu -> %llu)",
                           (unsigned long long)dl->total_size, (unsigned long long)r->content_length);
            seg_fail(seg, 1, 0);
            return;
        }
        seg->body_left = r->has_content_length ? r->content_length : UINT64_MAX;
    }

    seg->chunked = r->chunked;
    if (seg->chunked) {
        seg->body_left   = UINT64_MAX;
        seg->chunk_state = CH_SIZE;
        seg->chunk_left  = 0;
    }

    dl->redirects    = 0;                         /* a real response ends any redirect chain */
    seg_clear_refusals(seg);                      /* ...and the server's pushback */
    seg->state       = SEG_BODY;
    seg->deadline_ms = flux_now_ms() + FLUX_CONN_RECV_TOMS;
    seg_epoll(seg, EPOLLIN);
    seg_pump(seg);                                /* body bytes may already be buffered */
}

static void seg_on_headers(flux_segment_t *seg) {
    for (;;) {
        int rc = flux_http_read_head(seg->conn, seg->response);
        if (rc < 0) { seg_fail(seg, 0, SEG_BACKOFF); return; }
        if (rc == 1) { seg_headers_complete(seg); return; }

        ssize_t n = flux_conn_fill(seg->conn);
        if (n > 0) continue;
        if (n == 0) {
            flux_set_error(FLUX_ERR_NETWORK, "connection closed before the response headers arrived");
            seg_fail(seg, 0, SEG_BACKOFF);
            return;
        }
        if (would_block()) return;
        seg_fail(seg, 0, SEG_BACKOFF);
        return;
    }
}

int flux_seg_adopt(flux_segment_t *seg, int epoll_fd, flux_conn_t *conn,
                   const http_response_t *resp) {
    seg->epoll_fd = epoll_fd;
    seg_close_conn(seg);
    seg->conn = conn;
    seg->epoll_events = 0;

    uint64_t now = flux_now_ms();
    if (seg->started_at_ms == 0) seg->started_at_ms = now;
    seg->last_active_ms  = now;
    seg->speed_sample_ms = now;
    seg->last_flush_ms   = now;

    flux_gate_hold(seg->dl->target.host, seg->dl->target.port, now, &seg->gate);

    if (!seg->wbuf) {
        seg->wbuf = malloc(FLUX_WRITE_BUF_CAP);
        if (!seg->wbuf) {
            flux_set_error(FLUX_ERR_NOMEM, "segment %u: cannot allocate write buffer", seg->index);
            seg_fail(seg, 1, 0);
            return -1;
        }
    }

    *seg->response  = *resp;
    seg->req_offset = 0;
    seg->req_ranged = 0;               /* the probe's Range: 0-0 was ignored: this is the whole file */
    seg->state      = SEG_HEADERS;
    seg_epoll(seg, EPOLLIN);
    seg_headers_complete(seg);
    return seg->state == SEG_ERROR ? -1 : 0;
}

/* ── Event / timer entry points ───────────────────────────────────────────── */

void flux_seg_on_event(flux_segment_t *seg, uint32_t events) {
    (void)events;                       /* every handler just tries its I/O and reads the real error */
    if (!seg->conn) return;             /* stale event for a connection closed earlier in this batch */

    switch (seg->state) {
    case SEG_CONNECTING:
    case SEG_TLS_SHAKE: seg_on_connecting(seg); break;
    case SEG_REQUESTING: seg_send_request(seg); break;
    case SEG_HEADERS:    seg_on_headers(seg);   break;
    case SEG_BODY:       seg_pump(seg);         break;
    default: break;
    }
}

void flux_seg_tick(flux_segment_t *seg, uint64_t now) {
    switch (seg->state) {

    case SEG_RETRYING:
        if (now >= seg->retry_at_ms) flux_seg_start(seg, seg->epoll_fd);
        return;

    case SEG_THROTTLED:
        if (now >= seg->throttle_until_ms) {
            seg->state       = SEG_BODY;
            seg->deadline_ms = now + FLUX_CONN_RECV_TOMS;
            seg_epoll(seg, EPOLLIN);
            seg_pump(seg);              /* TLS/rbuf may hold data the socket no longer signals */
        }
        break;

    case SEG_BODY:
        if (seg->pump_again) { seg->pump_again = 0; seg_pump(seg); }
        break;

    default:
        break;
    }

    /* Timeouts and idle flushing apply to every live state (and may have been
     * made moot by the work above). */
    switch (seg->state) {
    case SEG_CONNECTING:
    case SEG_TLS_SHAKE:
    case SEG_REQUESTING:
    case SEG_HEADERS:
    case SEG_BODY:
        if (seg->deadline_ms && now >= seg->deadline_ms) {
            flux_set_error(FLUX_ERR_TIMEOUT_, "no data for %u s (state %d)",
                           (unsigned)(FLUX_CONN_RECV_TOMS / 1000), (int)seg->state);
            seg_fail(seg, 0, SEG_BACKOFF);
            return;
        }
        /* fallthrough */
    case SEG_THROTTLED:
        if (seg->wbuf_len > 0 && now - seg->last_flush_ms >= SEG_FLUSH_IDLE_MS) {
            if (flux_seg_flush(seg) < 0) seg_fail(seg, 1, 0);
        }
        break;
    default:
        break;
    }
}

uint64_t flux_seg_next_deadline(const flux_segment_t *seg) {
    uint64_t d = UINT64_MAX;
    switch (seg->state) {
    case SEG_RETRYING:
        return seg->retry_at_ms;
    case SEG_THROTTLED:
        d = seg->throttle_until_ms;
        break;
    case SEG_CONNECTING:
    case SEG_TLS_SHAKE:
    case SEG_REQUESTING:
    case SEG_HEADERS:
    case SEG_BODY:
        if (seg->pump_again) return 0;
        if (seg->deadline_ms) d = seg->deadline_ms;
        break;
    default:
        return UINT64_MAX;
    }
    if (seg->wbuf_len > 0) {
        uint64_t f = seg->last_flush_ms + SEG_FLUSH_IDLE_MS;
        if (f < d) d = f;
    }
    return d;
}

/* ── Stop ─────────────────────────────────────────────────────────────────── */

int flux_seg_halt(flux_segment_t *seg) {
    int rc = 0;
    if (seg->wbuf_len > 0 && flux_seg_flush(seg) < 0) rc = -1;
    seg_close_conn(seg);
    if (seg->state != SEG_COMPLETE && seg->state != SEG_ERROR) seg->state = SEG_PENDING;
    seg->deadline_ms = 0;
    seg->pump_again  = 0;
    seg_release_wbuf(seg);
    return rc;
}
