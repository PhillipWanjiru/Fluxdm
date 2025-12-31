/* FluxDM — core/include/segment.h
 * A download is split into N segments. Each segment owns one byte range and
 * one TCP (optionally TLS) connection. All segments of a download are driven
 * by that download's single worker thread from one epoll loop, so segment
 * code never needs locking — see downloader.h for the threading model.
 *
 * Accounting rule (the one every other rule hangs off):
 *
 *     received = written + wbuf_len
 *
 * `written` bytes are in the file, `wbuf_len` bytes sit in the write buffer
 * waiting for the next pwrite(). The next byte the segment expects belongs at
 * file offset range_start + received, and a (re)request after a failure always
 * starts there. Using `written` alone would re-download — and double-count —
 * whatever is still buffered.
 */
#pragma once

#include "connection.h"
#include "hostgate.h"
#include "protocol/http.h"
#include <stdint.h>
#include <stddef.h>

#define FLUX_MAX_SEGMENTS  64
#define FLUX_WRITE_BUF_CAP (256 * 1024)    /* max bytes buffered before pwrite */

/* ── Segment state machine ────────────────────────────────────────────────── */
typedef enum {
    SEG_PENDING      = 0,   /* idle: needs a (re)start                     */
    SEG_CONNECTING   = 1,   /* TCP connect in progress                     */
    SEG_TLS_SHAKE    = 2,   /* TLS handshake in progress                   */
    SEG_REQUESTING   = 3,   /* HTTP request being written                  */
    SEG_HEADERS      = 4,   /* waiting for / reading the response head     */
    SEG_BODY         = 5,   /* streaming the body to disk                  */
    SEG_PAUSED       = 6,   /* reserved (pausing is done per download)     */
    SEG_COMPLETE     = 7,   /* every byte of the range is on disk          */
    SEG_ERROR        = 8,   /* permanent failure — see err_code / err_msg  */
    SEG_RETRYING     = 9,   /* backing off until retry_at_ms               */
    SEG_THROTTLED    = 10,  /* body paused by the bandwidth limiter        */
} seg_state_t;

/* Chunked transfer-encoding decoder states */
typedef enum {
    CH_SIZE    = 0,         /* expecting "<hex>[;ext]\r\n"                 */
    CH_DATA    = 1,         /* inside a chunk's payload                    */
    CH_CRLF    = 2,         /* expecting the CRLF that ends a chunk        */
    CH_TRAILER = 3,         /* after the 0-size chunk: skip trailer lines  */
    CH_DONE    = 4,         /* terminating blank line seen                 */
} seg_chunk_state_t;

/* ── Segment ──────────────────────────────────────────────────────────────── */
typedef struct flux_segment {
    uint8_t      index;         /* 0-based segment number             */
    seg_state_t  state;

    /* Byte range */
    uint64_t     range_start;   /* inclusive                          */
    uint64_t     range_end;     /* inclusive, UINT64_MAX = open-ended */
    uint64_t     written;       /* bytes already pwrite()n to the file */

    /* I/O */
    flux_conn_t *conn;
    int          epoll_fd;      /* the download's epoll fd             */
    uint32_t     epoll_events;  /* interest mask currently armed (0 = not registered) */

    /* Write buffer — coalesces socket reads into large pwrite()s */
    uint8_t     *wbuf;
    size_t       wbuf_len;

    /* File descriptor shared with the parent download (pwrite, no seeking) */
    int          file_fd;

    /* Retry control */
    uint8_t      retries;       /* consecutive failures without progress */
    uint8_t      max_retries;
    uint32_t     retry_delay_ms;
    uint64_t     retry_at_ms;   /* when state == SEG_RETRYING           */
    uint64_t     progress_mark; /* `received` when retries was last reset */

    /* Server pushback (HTTP 429 / 503). Not a fault of the connection, so it does
     * not spend `retries`: the segment keeps asking, politely, within a time
     * budget (see hostgate.h). `gate` is the slot its connection holds. */
    flux_hostgate_t *gate;      /* NULL = no slot held                  */
    uint64_t     refused_since_ms;  /* first refusal of the current run, 0 = none */
    uint8_t      refusals;      /* refusals in the current run          */

    /* Timing (monotonic clock, ms) */
    uint64_t     started_at_ms;
    uint64_t     last_active_ms;
    uint64_t     deadline_ms;   /* stall/connect timeout, 0 = none      */
    uint64_t     throttle_until_ms;
    uint64_t     last_flush_ms;
    uint8_t      pump_again;    /* read budget ran out: pump again next loop turn */

    /* Speed sampling */
    uint64_t     speed_sample_bytes;
    uint64_t     speed_sample_ms;
    uint32_t     speed_bps;     /* bytes/sec rolling avg              */

    /* The request in flight */
    char         req[FLUX_MAX_URL + 1536];
    size_t       req_len;
    size_t       req_sent;
    uint64_t     req_offset;    /* file offset the response must start at */
    uint8_t      req_ranged;    /* the request carried a Range header     */
    uint32_t     req_gen;       /* download target generation it was sent to */

    /* Response framing */
    http_response_t *response;  /* heap-allocated, one per segment    */
    uint64_t     body_left;     /* identity bodies: bytes still expected,
                                 * UINT64_MAX = read until the peer closes */
    uint8_t      chunked;
    uint8_t      chunk_state;   /* seg_chunk_state_t                  */
    uint64_t     chunk_left;    /* bytes left in the current chunk     */

    /* Why the segment failed (valid in SEG_ERROR, and informational while
     * SEG_RETRYING) */
    flux_error_t err_code;
    char         err_msg[160];

    /* Parent back-pointer */
    struct flux_download *dl;
} flux_segment_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Allocate and initialise a segment. Does not connect yet. */
flux_segment_t *flux_seg_new(struct flux_download *dl, uint8_t index,
                              uint64_t start, uint64_t end);

/* Start (or restart) the segment: open a connection to the download's current
 * target and register it with `epoll_fd`. Returns 0 when the segment is under
 * way or a retry has been scheduled — including "the host cannot take another
 * connection right now" (the segment then waits in SEG_RETRYING for a free
 * slot) — and -1 when it failed permanently. */
int  flux_seg_start(flux_segment_t *seg, int epoll_fd);

/* Take over an already-connected socket whose response head has been parsed
 * (the probe's connection when the server ignores Range and answers 200):
 * saves a whole connect + TLS + request round trip for single-stream
 * downloads. The segment owns `conn` from here on, even on failure. */
int  flux_seg_adopt(flux_segment_t *seg, int epoll_fd, flux_conn_t *conn,
                    const http_response_t *resp);

/* ── Driving ──────────────────────────────────────────────────────────────── */

/* The segment's socket became ready (call with the epoll event mask). Check
 * seg->state afterwards: SEG_COMPLETE / SEG_ERROR are terminal. */
void flux_seg_on_event(flux_segment_t *seg, uint32_t events);

/* Time-driven work: retry back-off expiry, end of a bandwidth throttle,
 * stall / connect timeouts, periodic flushing of idle buffers. */
void flux_seg_tick(flux_segment_t *seg, uint64_t now_ms);

/* Absolute time (ms, monotonic) at which flux_seg_tick() next has work to do;
 * UINT64_MAX if the segment only waits for socket events. */
uint64_t flux_seg_next_deadline(const flux_segment_t *seg);

/* Flush the write buffer to disk via pwrite(). 0 on success. */
int  flux_seg_flush(flux_segment_t *seg);

/* Stop: flush, close the connection, return to SEG_PENDING keeping all
 * progress (used for pause and shutdown). Returns -1 if the flush failed. */
int  flux_seg_halt(flux_segment_t *seg);

/* Abort: close the connection and DISCARD buffered bytes. */
void flux_seg_abort(flux_segment_t *seg);

/* Free all resources. Does NOT write the pending buffer — halt first. */
void flux_seg_free(flux_segment_t *seg);

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Bytes received so far: written + buffered. */
uint64_t flux_seg_received(const flux_segment_t *seg);
/* Bytes still needed to finish the range (UINT64_MAX for open-ended). */
uint64_t flux_seg_remaining(const flux_segment_t *seg);
/* File offset of the next byte expected. */
uint64_t flux_seg_offset(const flux_segment_t *seg);
void     flux_seg_update_speed(flux_segment_t *seg, uint64_t now_ms, size_t bytes);
