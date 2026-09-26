/* FluxDM — core/include/protocol/http.h
 * HTTP/1.1 request building and response parsing.
 * Supports: GET, HEAD, byte-range requests, chunked transfer, redirects.
 */
#pragma once

/* Self-contained size constants — also defined in ipc/protocol.h.
 * Guard prevents redefinition when both are included. */
#ifndef FLUX_MAX_URL
#  define FLUX_MAX_URL  4096
#endif
#ifndef FLUX_MAX_PATH
#  define FLUX_MAX_PATH 1024
#endif

#include "../connection.h"
#include "../errors.h"
#include <stdint.h>
#include <stddef.h>

/* Forward declarations — avoids circular include with segment.h
 * (segment.h embeds a pointer to http_response_t, http.h uses
 * flux_segment_t* in its prototypes — neither needs the other's
 * full definition at header-parse time). */
struct flux_segment;
typedef struct flux_segment flux_segment_t;

#define HTTP_MAX_HEADERS     64
#define HTTP_MAX_HEADER_LEN  8192
#define HTTP_MAX_REDIRECTS   10
#define HTTP_VERSION         "HTTP/1.1"
#define HTTP_USER_AGENT      "FluxDM/1.0 (+https://github.com/fluxdm)"

/* ── Parsed HTTP response ─────────────────────────────────────────────────── */
typedef struct http_response {
    int      status;                    /* 200, 206, 301, 404 … */
    char     reason[64];

    uint64_t content_length;            /* 0 if unknown / chunked */
    uint64_t content_range_start;
    uint64_t content_range_end;
    uint64_t content_range_total;       /* 0 if not provided      */

    uint8_t  chunked;                   /* Transfer-Encoding: chunked */
    uint8_t  keepalive;
    uint8_t  accept_ranges;             /* 1 if server supports ranges */

    char     location[FLUX_MAX_URL];    /* Redirect target             */
    char     content_type[128];
    char     etag[128];
    char     last_modified[64];

    /* Raw header block (for debug / extension) */
    char     raw[HTTP_MAX_HEADER_LEN];
    size_t   raw_len;
    size_t   header_end;                /* offset into raw where body starts */
} http_response_t;

/* ── URL components ───────────────────────────────────────────────────────── */
typedef struct {
    char     scheme[8];       /* "http" or "https"     */
    char     host[256];
    uint16_t port;
    char     path[FLUX_MAX_URL];
} http_url_t;

/* ── API ──────────────────────────────────────────────────────────────────── */

/* Parse a URL into components. Returns 0 on success. */
int  http_url_parse(const char *url, http_url_t *out);

/* Build and send an HTTP GET request with a byte-range header.
 * range_end = UINT64_MAX means "to end of file" (no upper bound). */
int  flux_http_send_range_request(flux_segment_t *seg);

/* Build and send an HTTP HEAD request to probe file size and range support. */
int  flux_http_send_head(flux_conn_t *conn, const http_url_t *url);

/* Drive the HTTP header parser. Call whenever data arrives in seg->conn.
 * Returns:  1 = headers complete (body may have started in ring buffer)
 *           0 = incomplete, call again when more data arrives
 *          -1 = parse error                                              */
int  flux_http_parse_headers(flux_segment_t *seg);

/* Parse a HEAD response from a fully-buffered conn. */
int  flux_http_parse_head_response(flux_conn_t *conn, http_response_t *out);

/* Consume a chunked-encoding chunk header from the ring buffer.
 * Returns chunk size (0 = last chunk), -1 on error. */
int64_t flux_http_next_chunk_size(flux_conn_t *conn);

/* Map an HTTP status code to a flux_error_t. */
flux_error_t http_status_to_flux_error(int status);
