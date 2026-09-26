/* FluxDM — core/src/segment.c */

#include "../include/segment.h"
#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/protocol/http.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <time.h>
#include <errno.h>

/* ── Monotonic clock helper ───────────────────────────────────────────────── */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

/* ── Epoll arm/disarm ─────────────────────────────────────────────────────── */
static int epoll_set(flux_segment_t *seg, int op, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.ptr = seg };
    return epoll_ctl(seg->epoll_fd, op, seg->conn->fd, &ev);
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

flux_segment_t *flux_seg_new(struct flux_download *dl, uint8_t index,
                              uint64_t start, uint64_t end) {
    flux_segment_t *seg = calloc(1, sizeof(*seg));
    if (!seg) {
        flux_set_error(FLUX_ERR_NOMEM, "flux_seg_new: calloc");
        return NULL;
    }
    seg->wbuf = malloc(FLUX_WRITE_BUF_CAP);
    if (!seg->wbuf) {
        free(seg);
        flux_set_error(FLUX_ERR_NOMEM, "flux_seg_new: wbuf malloc");
        return NULL;
    }

    seg->response = calloc(1, sizeof(http_response_t));
    if (!seg->response) {
        free(seg->wbuf);
        free(seg);
        flux_set_error(FLUX_ERR_NOMEM, "flux_seg_new: response calloc");
        return NULL;
    }

    seg->index           = index;
    seg->state           = SEG_PENDING;
    seg->range_start     = start;
    seg->range_end       = end;
    seg->written         = 0;
    seg->dl              = dl;
    seg->file_fd         = dl->file_fd;
    seg->epoll_fd        = -1;
    seg->max_retries     = 5;
    seg->retry_delay_ms  = 500;
    return seg;
}

int flux_seg_start(flux_segment_t *seg, int epoll_fd) {
    seg->epoll_fd    = epoll_fd;
    seg->started_at_ms = now_ms();
    seg->last_active_ms = seg->started_at_ms;

    /* Get a connection from parent's pool or create a new one */
    flux_conn_pool_t *pool = &seg->dl->conn_pool;
    int tls = seg->dl->use_tls;
    seg->conn = flux_pool_acquire(pool, seg->dl->host, seg->dl->port, tls);

    if (!seg->conn) {
        seg->conn = flux_conn_new(seg->dl->host, seg->dl->port, tls);
        if (!seg->conn) return -1;

        if (flux_conn_start_connect(seg->conn) < 0) {
            flux_conn_close(seg->conn);
            seg->conn = NULL;
            return -1;
        }
    }

    if (seg->conn->state == CONN_STATE_READY) {
        /* Already connected (pooled) — send request immediately */
        seg->state = SEG_REQUESTING;
        extern int flux_http_send_range_request(flux_segment_t *seg);
        if (flux_http_send_range_request(seg) < 0) return -1;
        epoll_set(seg, EPOLL_CTL_ADD, EPOLLIN);
        seg->epoll_events = EPOLLIN;
    } else {
        /* Non-blocking connect in progress */
        seg->state = SEG_CONNECTING;
        epoll_set(seg, EPOLL_CTL_ADD, EPOLLOUT);
        seg->epoll_events = EPOLLOUT;
    }

    return 0;
}

/* ── Drive the state machine ──────────────────────────────────────────────── */
int flux_seg_drive(flux_segment_t *seg, uint32_t events) {
    seg->last_active_ms = now_ms();

    switch (seg->state) {

    case SEG_CONNECTING: {
        if (events & (EPOLLERR | EPOLLHUP)) {
            flux_set_error(FLUX_ERR_CONNECT, "seg %u: connect error", seg->index);
            goto retry;
        }
        int rc = flux_conn_finish_connect(seg->conn);
        if (rc < 0) goto retry;

        if (rc == 0 || rc == 2) {
            /* TCP connected, but the TLS handshake needs more round
             * trips before it's done — real handshakes almost always
             * do. Move to a dedicated waiting state and re-arm epoll
             * for exactly the direction OpenSSL asked for. Arming the
             * wrong direction (or, as before this fix, not tracking
             * this at all and assuming one step always finished the
             * handshake) is what silently hung every HTTPS segment
             * forever with no error and no progress. */
            seg->state = SEG_TLS_SHAKE;
            uint32_t want = (rc == 2) ? EPOLLOUT : EPOLLIN;
            epoll_set(seg, EPOLL_CTL_MOD, want);
            seg->epoll_events = want;
            break;
        }

        /* rc == 1: fully connected, TLS done or not needed */
        extern int flux_http_send_range_request(flux_segment_t *seg);
        if (flux_http_send_range_request(seg) < 0) goto retry;

        seg->state = SEG_REQUESTING;
        epoll_set(seg, EPOLL_CTL_MOD, EPOLLIN);
        seg->epoll_events = EPOLLIN;
        break;
    }

    case SEG_TLS_SHAKE: {
        if (events & (EPOLLERR | EPOLLHUP)) {
            flux_set_error(FLUX_ERR_TLS_HNDSHK,
                           "seg %u: connection error during TLS handshake", seg->index);
            goto retry;
        }
        extern int flux_conn_tls_handshake_continue(flux_conn_t *c);
        int rc = flux_conn_tls_handshake_continue(seg->conn);
        if (rc < 0) goto retry;

        if (rc == 0 || rc == 2) {
            /* Still not done. TLS 1.3 in particular can flip between
             * wanting read and write across steps, so re-check which
             * direction is needed each time rather than assuming it
             * stays the same as the last step. */
            uint32_t want = (rc == 2) ? EPOLLOUT : EPOLLIN;
            epoll_set(seg, EPOLL_CTL_MOD, want);
            seg->epoll_events = want;
            break;
        }

        /* rc == 1: handshake complete, connection is READY */
        extern int flux_http_send_range_request(flux_segment_t *seg);
        if (flux_http_send_range_request(seg) < 0) goto retry;

        seg->state = SEG_REQUESTING;
        epoll_set(seg, EPOLL_CTL_MOD, EPOLLIN);
        seg->epoll_events = EPOLLIN;
        break;
    }

    case SEG_REQUESTING:
    case SEG_HEADERS: {
        if (events & (EPOLLERR | EPOLLHUP)) goto retry;

        ssize_t n = flux_conn_fill(seg->conn);
        if (n < 0 && flux_last_error() != FLUX_ERR_WOULDBLOCK) goto retry;
        if (n == 0) goto retry;   /* EOF before headers complete */

        extern int flux_http_parse_headers(flux_segment_t *seg);
        int rc = flux_http_parse_headers(seg);
        if (rc < 0) goto retry;
        if (rc == 0) break;       /* headers incomplete, wait for more data */

        /* Headers done: any buffered bytes are body */
        seg->state = SEG_BODY;
        /* fall through to body handler */
    }
    /* fall through */
    case SEG_BODY: {
        if (events & EPOLLERR) goto retry;
        /* EPOLLHUP = peer closed (TCP FIN, normal end of an open-ended
         * transfer). It is NOT an error here — drain whatever is left,
         * then let the n==0 path below detect completion. */
        uint32_t _hup = (events & EPOLLHUP);

        /* Drain buffered data first */
        while (flux_conn_buffered(seg->conn) > 0) {
            size_t avail = flux_conn_buffered(seg->conn);
            uint64_t remaining = flux_seg_remaining(seg);

            /* Clamp to what the segment needs */
            if (remaining != UINT64_MAX && avail > (size_t)remaining)
                avail = (size_t)remaining;

            size_t space = FLUX_WRITE_BUF_CAP - seg->wbuf_len;
            size_t take  = avail < space ? avail : space;

            flux_conn_drain(seg->conn, seg->wbuf + seg->wbuf_len, take);
            seg->wbuf_len += take;
            flux_seg_update_speed(seg, now_ms(), take);

            /* Apply bandwidth token check */
            extern int flux_dl_bw_consume(struct flux_download *dl, size_t bytes);
            if (flux_dl_bw_consume(seg->dl, take) < 0) {
                /* Throttled: flush what we have, pause this segment */
                flux_seg_flush(seg);
                flux_seg_pause(seg);
                return 0;
            }

            /* Flush when buffer is full or segment is done */
            if (seg->wbuf_len >= FLUX_WRITE_BUF_CAP ||
                flux_seg_remaining(seg) == 0) {
                if (flux_seg_flush(seg) < 0) return -1;
            }
        }

        /* Read more from socket (also triggered by HUP for one final drain) */
        if ((events & EPOLLIN) || _hup) {
            ssize_t n = flux_conn_fill(seg->conn);
            if (n < 0 && flux_last_error() != FLUX_ERR_WOULDBLOCK) goto retry;
            if (n == 0) {
                /* Flush whatever is left in the write buffer */
                if (flux_seg_flush(seg) < 0) return -1;

                if (seg->range_end == UINT64_MAX) {
                    /* Open-ended segment: server EOF = download complete */
                    seg->state = SEG_COMPLETE;
                    flux_pool_release(&seg->dl->conn_pool, seg->conn);
                    seg->conn = NULL;
                    return 1;
                }
                /* Known range but premature EOF — retry */
                goto retry;
            }
        }

        /* Check completion (ranged segments: remaining bytes hit zero) */
        if (flux_seg_remaining(seg) == 0) {
            flux_seg_flush(seg);
            seg->state = SEG_COMPLETE;
            /* Return connection to pool */
            flux_pool_release(&seg->dl->conn_pool, seg->conn);
            seg->conn = NULL;
            return 1;   /* segment done */
        }
        break;
    }

    default: break;
    }

    return 0;

retry:
    if (seg->retries < seg->max_retries) {
        seg->retries++;
        flux_conn_close(seg->conn); seg->conn = NULL;
        seg->state = SEG_RETRYING;
        /* Caller (downloader.c) will restart after retry_delay_ms */
        return 0;
    }
    seg->state = SEG_ERROR;
    return -1;
}

/* ── Write buffer flush ───────────────────────────────────────────────────── */
int flux_seg_flush(flux_segment_t *seg) {
    if (seg->wbuf_len == 0) return 0;

    uint64_t offset = seg->range_start + seg->written;
    ssize_t  n = pwrite(seg->file_fd, seg->wbuf, seg->wbuf_len, (off_t)offset);

    if (n < 0) {
        flux_set_syserror("pwrite");
        flux_set_error(FLUX_ERR_DISK_WRITE, "pwrite failed at offset %lu", offset);
        return -1;
    }
    seg->written   += (uint64_t)n;
    seg->wbuf_len   = 0;

    /* Notify parent of progress */
    extern void flux_dl_on_progress(struct flux_download *dl, uint64_t bytes);
    flux_dl_on_progress(seg->dl, (uint64_t)n);
    return 0;
}

/* ── Pause / Resume ───────────────────────────────────────────────────────── */
int flux_seg_pause(flux_segment_t *seg) {
    if (seg->state == SEG_PAUSED) return 0;
    if (seg->conn && seg->conn->fd >= 0) {
        epoll_ctl(seg->epoll_fd, EPOLL_CTL_DEL, seg->conn->fd, NULL);
        seg->epoll_events = 0;
    }
    seg->state = SEG_PAUSED;
    return 0;
}

int flux_seg_resume(flux_segment_t *seg) {
    if (seg->state != SEG_PAUSED) return 0;

    if (!seg->conn || seg->conn->state != CONN_STATE_READY) {
        /* Connection was lost — reconnect */
        if (seg->conn) { flux_conn_close(seg->conn); seg->conn = NULL; }
        return flux_seg_start(seg, seg->epoll_fd);
    }

    seg->state = SEG_BODY;
    epoll_set(seg, EPOLL_CTL_ADD, EPOLLIN);
    seg->epoll_events = EPOLLIN;
    return 0;
}

/* ── Abort ────────────────────────────────────────────────────────────────── */
void flux_seg_abort(flux_segment_t *seg) {
    if (seg->conn) {
        if (seg->conn->fd >= 0 && seg->epoll_fd >= 0)
            epoll_ctl(seg->epoll_fd, EPOLL_CTL_DEL, seg->conn->fd, NULL);
        flux_conn_close(seg->conn);
        seg->conn = NULL;
    }
    seg->wbuf_len = 0;
    seg->state    = SEG_PENDING;
}

/* ── Free ─────────────────────────────────────────────────────────────────── */
void flux_seg_free(flux_segment_t *seg) {
    if (!seg) return;
    flux_seg_abort(seg);
    free(seg->wbuf);
    free(seg->response);
    free(seg);
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */
uint64_t flux_seg_remaining(const flux_segment_t *seg) {
    if (seg->range_end == UINT64_MAX) return UINT64_MAX;
    uint64_t total = seg->range_end - seg->range_start + 1u;
    return total > seg->written ? total - seg->written : 0u;
}

uint64_t flux_seg_offset(const flux_segment_t *seg) {
    return seg->range_start + seg->written;
}

void flux_seg_update_speed(flux_segment_t *seg, uint64_t now, size_t bytes) {
    seg->speed_sample_bytes += bytes;
    uint64_t elapsed = now - seg->speed_sample_ms;
    if (elapsed >= 500) {   /* update every 500 ms */
        seg->speed_bps        = (uint32_t)(seg->speed_sample_bytes * 1000u / elapsed);
        seg->speed_sample_bytes = 0;
        seg->speed_sample_ms  = now;
    }
}
