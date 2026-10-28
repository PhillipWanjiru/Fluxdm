/* FluxDM — core/src/ipc/frame_io.c */

#include "frame_io.h"
#include <stdlib.h>
#include <string.h>

#define READER_CHUNK        4096u            /* minimum free space offered to read(2)  */
#define READER_KEEP_CAP     (64u * 1024u)    /* an idle reader gives back anything larger */
#define FRAME_MAX_TOTAL     (sizeof(flux_header_t) + FLUX_MAX_PAYLOAD)

/* The wire is little-endian and the header is copied byte for byte. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#  error "frame_io.c assumes a little-endian host"
#endif

/* ── Encoding ─────────────────────────────────────────────────────────────── */

size_t flux_frame_encode(uint8_t *out, size_t cap, flux_msg_type_t type, uint16_t flags,
                         uint32_t id, const void *payload, uint32_t payload_len) {
    if (payload_len > FLUX_MAX_PAYLOAD) return 0;
    size_t total = sizeof(flux_header_t) + payload_len;
    if (cap < total) return 0;

    flux_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic       = FLUX_MAGIC;
    h.version     = (uint8_t)FLUX_PROTO_VER;
    h.type        = (uint8_t)type;
    h.flags       = flags;
    h.id          = id;
    h.payload_len = payload_len;

    memcpy(out, &h, sizeof(h));
    if (payload_len > 0 && payload) memcpy(out + sizeof(h), payload, payload_len);
    return total;
}

/* ── Reader ───────────────────────────────────────────────────────────────── */

void flux_frame_reader_init(flux_frame_reader_t *r) {
    memset(r, 0, sizeof(*r));
}

void flux_frame_reader_free(flux_frame_reader_t *r) {
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

int flux_frame_reader_reserve(flux_frame_reader_t *r, uint8_t **ptr, size_t *space) {
    size_t used = r->len - r->start;

    /* Nothing pending: start over at the front of the buffer, and don't keep a
     * megabyte alive for a client that sent one big frame once. */
    if (used == 0) {
        r->start = r->len = 0;
        if (r->cap > READER_KEEP_CAP) { free(r->buf); r->buf = NULL; r->cap = 0; }
    }

    /* How much room do we need behind the unconsumed bytes? Enough for another
     * chunk, and — once a header says how large the frame is — for all of it. */
    size_t want = used + READER_CHUNK;
    if (used >= sizeof(flux_header_t)) {
        flux_header_t h;
        memcpy(&h, r->buf + r->start, sizeof(h));
        if (h.payload_len <= FLUX_MAX_PAYLOAD) {
            size_t total = sizeof(flux_header_t) + h.payload_len;
            if (total > want) want = total;
        }
    }
    if (want > FRAME_MAX_TOTAL + READER_CHUNK) want = FRAME_MAX_TOTAL + READER_CHUNK;

    if (r->cap - r->start < want || r->cap - r->len < READER_CHUNK) {
        /* Slide the unconsumed bytes to the front ... */
        if (r->start > 0) {
            memmove(r->buf, r->buf + r->start, used);
            r->len   = used;
            r->start = 0;
        }
        /* ... and grow if that was not enough. */
        if (r->cap < want) {
            size_t ncap = r->cap ? r->cap : READER_CHUNK;
            while (ncap < want) ncap *= 2;
            uint8_t *nb = realloc(r->buf, ncap);
            if (!nb) return -1;
            r->buf = nb;
            r->cap = ncap;
        }
    }

    *ptr   = r->buf + r->len;
    *space = r->cap - r->len;
    return 0;
}

void flux_frame_reader_commit(flux_frame_reader_t *r, size_t n) {
    r->len += n;
}

int flux_frame_reader_feed(flux_frame_reader_t *r, const void *data, size_t n) {
    const uint8_t *src = data;
    while (n > 0) {
        uint8_t *dst;
        size_t   space;
        if (flux_frame_reader_reserve(r, &dst, &space) < 0) return -1;
        size_t take = n < space ? n : space;
        memcpy(dst, src, take);
        flux_frame_reader_commit(r, take);
        src += take;
        n   -= take;
    }
    return 0;
}

frame_status_t flux_frame_reader_next(flux_frame_reader_t *r, flux_header_t *hdr,
                                      const uint8_t **payload) {
    size_t avail = r->len - r->start;
    if (avail < sizeof(flux_header_t)) return FRAME_NEED_MORE;

    memcpy(hdr, r->buf + r->start, sizeof(*hdr));
    if (hdr->magic != FLUX_MAGIC || hdr->version != FLUX_PROTO_VER ||
        hdr->payload_len > FLUX_MAX_PAYLOAD)
        return FRAME_BAD;

    size_t total = sizeof(flux_header_t) + hdr->payload_len;
    if (avail < total) return FRAME_NEED_MORE;

    *payload  = r->buf + r->start + sizeof(flux_header_t);
    r->start += total;
    return FRAME_READY;
}
