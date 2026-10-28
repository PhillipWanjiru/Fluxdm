/* FluxDM — core/src/ipc/frame_io.h
 * Pure framing for the core <-> daemon protocol: an encoder and an incremental
 * reader. No sockets, no threads, no globals — so the exact byte layout and the
 * handling of split, coalesced, oversized and garbage input can be unit-tested
 * without a network.
 *
 * Wire format (little-endian): flux_header_t (16 bytes) + payload_len bytes.
 */
#pragma once

#include "../../../ipc/protocol.h"
#include <stddef.h>
#include <stdint.h>

/* ── Encoding ─────────────────────────────────────────────────────────────── */

/* Write header + payload into `out`. Returns the frame length, or 0 if `cap`
 * is too small or the payload exceeds FLUX_MAX_PAYLOAD. */
size_t flux_frame_encode(uint8_t *out, size_t cap, flux_msg_type_t type, uint16_t flags,
                         uint32_t id, const void *payload, uint32_t payload_len);

/* ── Incremental reader ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;       /* heap, grows on demand up to one maximal frame      */
    size_t   cap;
    size_t   len;       /* bytes in buf (consumed + unconsumed)               */
    size_t   start;     /* offset of the first unconsumed byte                */
} flux_frame_reader_t;

typedef enum {
    FRAME_NEED_MORE = 0,    /* no complete frame buffered yet                   */
    FRAME_READY     = 1,    /* a frame was returned                             */
    FRAME_BAD       = -1,   /* garbage on the wire: the stream cannot be resynchronised */
} frame_status_t;

void flux_frame_reader_init(flux_frame_reader_t *r);
void flux_frame_reader_free(flux_frame_reader_t *r);

/* Make room for the next read(2): returns a pointer to writable space of at
 * least *space bytes (more when a large frame is pending). Invalidates
 * pointers returned by flux_frame_reader_next(). -1 on out-of-memory. */
int  flux_frame_reader_reserve(flux_frame_reader_t *r, uint8_t **ptr, size_t *space);

/* Declare that `n` bytes were written at the pointer from _reserve(). */
void flux_frame_reader_commit(flux_frame_reader_t *r, size_t n);

/* Append bytes (reserve + copy + commit). Convenient for tests. -1 on OOM. */
int  flux_frame_reader_feed(flux_frame_reader_t *r, const void *data, size_t n);

/* Extract the next complete frame. On FRAME_READY, *hdr is the parsed header
 * and *payload points INTO the reader's buffer (payload_len bytes, not aligned,
 * valid until the next _reserve/_feed). The frame is consumed. */
frame_status_t flux_frame_reader_next(flux_frame_reader_t *r, flux_header_t *hdr,
                                      const uint8_t **payload);
