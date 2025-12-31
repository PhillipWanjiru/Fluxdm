/* FluxDM — core/include/protocol/http.h
 * HTTP/1.1 request building, response-head parsing and URL handling.
 *
 * Pure functions wherever possible (no sockets): URL parsing/resolution, the
 * request builder and the head parser all work on plain buffers so they can
 * be unit-tested without a network. The only connection-aware helper is
 * flux_http_read_head(), which peeks the connection's read buffer.
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

#define HTTP_MAX_HEADER_LEN  16384   /* largest response head we accept   */
#define HTTP_MAX_REDIRECTS   10
#define HTTP_VERSION         "HTTP/1.1"
#define HTTP_USER_AGENT      "FluxDM/1.0 (+https://github.com/fluxdm)"

/* ── Parsed HTTP response head ────────────────────────────────────────────── */
typedef struct http_response {
    int      status;                    /* 200, 206, 301, 404 …           */
    char     reason[64];
    uint8_t  version_minor;             /* the x in HTTP/1.x              */

    uint8_t  has_content_length;
    uint64_t content_length;

    uint8_t  has_content_range;
    uint64_t content_range_start;
    uint64_t content_range_end;
    uint64_t content_range_total;       /* 0 if the server sent '*'       */
    uint8_t  content_range_unsat;       /* unsatisfied form of a 416      */

    uint8_t  chunked;                   /* Transfer-Encoding: chunked     */
    uint8_t  keepalive;                 /* connection may be reused       */
    uint8_t  accept_ranges;             /* 1 if the server supports ranges */
    uint32_t retry_after_s;             /* Retry-After in seconds, 0 = none */

    char     location[FLUX_MAX_URL];    /* redirect target                */
    char     content_type[128];
    char     etag[128];
    char     last_modified[64];
} http_response_t;

/* ── URL components ───────────────────────────────────────────────────────── */
typedef struct {
    char     scheme[8];       /* "http" or "https" (lower-cased)             */
    char     host[256];       /* without [] for IPv6 literals                */
    uint16_t port;
    char     path[FLUX_MAX_URL];   /* request-target: path + query, already
                                    * percent-encoded, always starts with '/' */
    char     userinfo[256];   /* "user:password" from the URL, or ""         */
} http_url_t;

/* ── URLs ─────────────────────────────────────────────────────────────────── */

/* Parse an http:// or https:// URL. Spaces and other characters that are
 * illegal in a request-target are percent-encoded (the way a browser does);
 * existing %XX escapes are kept; a #fragment is dropped.
 * Returns 0 on success, -1 with FLUX_ERR_HTTP_PARSE otherwise. */
int  http_url_parse(const char *url, http_url_t *out);

/* Resolve a Location header value (absolute, scheme-relative, absolute-path
 * or relative) against the URL it came from. Credentials embedded in `base`
 * are only inherited when the redirect stays on the same host. */
int  http_url_resolve(const http_url_t *base, const char *location, http_url_t *out);

/* 1 if the URL needs TLS. */
int  http_url_is_tls(const http_url_t *u);

/* "host" or "host:port" (port omitted when it is the scheme default; IPv6
 * literals bracketed) as it must appear in the Host header. */
int  http_host_header(const http_url_t *u, char *out, size_t cap);

/* ── Requests ─────────────────────────────────────────────────────────────── */

/* Build a GET request. When `ranged`, a Range header is added:
 * bytes=first-last, or bytes=first- when last == UINT64_MAX.
 * Always asks for identity encoding and Connection: close.
 * Returns the request length, or -1 if it does not fit in `cap`. */
int  http_build_get(char *buf, size_t cap, const http_url_t *u,
                    int ranged, uint64_t first, uint64_t last);

/* ── Responses ────────────────────────────────────────────────────────────── */

/* Parse a response head from a flat buffer (does not modify it).
 * Returns the number of bytes the head occupies (status line through the
 * blank line) when complete, 0 if more data is needed, -1 on a malformed
 * or oversized head (FLUX_ERR_HTTP_PARSE). `*r` is zeroed first. */
int  http_parse_head(http_response_t *r, const char *buf, size_t len);

/* Parse a head from the connection's read buffer and consume exactly the
 * head, leaving any body bytes buffered. 1 = done, 0 = incomplete, -1 = error. */
int  flux_http_read_head(flux_conn_t *conn, http_response_t *out);

/* Same as flux_http_read_head(); kept under its original name. */
int  flux_http_parse_head_response(flux_conn_t *conn, http_response_t *out);

/* Map an HTTP status code to a flux_error_t. */
flux_error_t http_status_to_flux_error(int status);

/* 1 for statuses worth retrying with back-off (408, 425, 429, 500, 502-504). */
int  http_status_is_retryable(int status);
