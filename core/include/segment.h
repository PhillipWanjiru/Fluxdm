/* FluxDM — core/include/segment.h
 * A download is split into N segments. Each segment owns a byte range
 * and one TCP connection. Segments run concurrently under the epoll loop.
 */
#pragma once

#include "connection.h"
#include <stdint.h>
#include <stddef.h>

/* Forward declaration — full definition in protocol/http.h.
 * Kept as a pointer field below so segment.h never needs to include
 * protocol/http.h (which itself forward-declares flux_segment_t). */
typedef struct http_response http_response_t;

#define FLUX_MAX_SEGMENTS  64
#define FLUX_WRITE_BUF_CAP (256 * 1024)    /* max bytes buffered before write */

/* ── Segment state machine ────────────────────────────────────────────────── */
typedef enum {
    SEG_PENDING      = 0,   /* waiting to be started              */
    SEG_CONNECTING   = 1,   /* TCP connect in progress            */
    SEG_TLS_SHAKE    = 2,   /* TLS handshake in progress          */
    SEG_REQUESTING   = 3,   /* HTTP request sent, awaiting header */
    SEG_HEADERS      = 4,   /* reading HTTP headers               */
    SEG_BODY         = 5,   /* streaming body to disk             */
    SEG_PAUSED       = 6,   /* user paused or bandwidth limit hit */
    SEG_COMPLETE     = 7,   /* segment finished                   */
    SEG_ERROR        = 8,   /* unrecoverable error                */
    SEG_RETRYING     = 9,   /* backing off before retry           */
} seg_state_t;

/* ── Segment ──────────────────────────────────────────────────────────────── */
typedef struct flux_segment {
    uint8_t      index;         /* 0-based segment number             */
    seg_state_t  state;

    /* Byte range */
    uint64_t     range_start;   /* inclusive                          */
    uint64_t     range_end;     /* inclusive, UINT64_MAX = open-ended */
    uint64_t     written;       /* bytes written so far               */

    /* I/O */
    flux_conn_t *conn;
    int          epoll_fd;      /* parent epoll fd (for re-arming)    */
    uint32_t     epoll_events;  /* current interest mask              */

    /* Write buffer — coalesce small writes before pwrite()           */
    uint8_t     *wbuf;
    size_t       wbuf_len;

    /* File descriptor (shared with parent download, seeked via pwrite) */
    int          file_fd;

    /* Retry control */
    uint8_t      retries;
    uint8_t      max_retries;
    uint32_t     retry_delay_ms;

    /* Timing (monotonic clock) */
    uint64_t     started_at_ms;
    uint64_t     last_active_ms;

    /* Speed sampling */
    uint64_t     speed_sample_bytes;
    uint64_t     speed_sample_ms;
    uint32_t     speed_bps;     /* bytes/sec rolling avg              */

    /* Per-segment HTTP response (isolated from other segments) */
    http_response_t     *response;  /* heap-allocated, one per segment */

    /* Parent back-pointer */
    struct flux_download *dl;
} flux_segment_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Allocate and initialise a segment. Does not connect yet. */
flux_segment_t *flux_seg_new(struct flux_download *dl, uint8_t index,
                              uint64_t start, uint64_t end);

/* Start the segment: initiate TCP connect, register with epoll. */
int  flux_seg_start(flux_segment_t *seg, int epoll_fd);

/* Drive the segment state machine. Call when epoll fires on seg->conn->fd.
 * Returns 1 if the segment completed, 0 if still running, -1 on error. */
int  flux_seg_drive(flux_segment_t *seg, uint32_t events);

/* Flush the write buffer to disk via pwrite(). */
int  flux_seg_flush(flux_segment_t *seg);

/* Pause: remove from epoll interest, buffer current position. */
int  flux_seg_pause(flux_segment_t *seg);

/* Resume: re-arm epoll. If connection was lost, reconnect. */
int  flux_seg_resume(flux_segment_t *seg);

/* Abort: close connection, discard buffers, leave state on disk for resume. */
void flux_seg_abort(flux_segment_t *seg);

/* Free all resources. Does NOT write pending buffer — call flush first. */
void flux_seg_free(flux_segment_t *seg);

/* ── Helpers ──────────────────────────────────────────────────────────────── */
uint64_t flux_seg_remaining(const flux_segment_t *seg);
uint64_t flux_seg_offset(const flux_segment_t *seg);   /* current write pos */
void     flux_seg_update_speed(flux_segment_t *seg, uint64_t now_ms, size_t bytes);
