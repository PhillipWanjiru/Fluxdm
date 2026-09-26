/* FluxDM — core/src/protocol/http.c */

#include "../../include/protocol/http.h"
#include "../../include/errors.h"
#include "../../include/connection.h"
#include "../../include/downloader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* ── URL parser ───────────────────────────────────────────────────────────── */
int http_url_parse(const char *url, http_url_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = url;

    /* Scheme */
    const char *colon = strstr(p, "://");
    if (!colon) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "URL missing scheme: %s", url);
        return -1;
    }
    size_t slen = (size_t)(colon - p);
    if (slen >= sizeof(out->scheme)) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "scheme too long");
        return -1;
    }
    memcpy(out->scheme, p, slen);
    p = colon + 3;

    /* Default port */
    out->port = (strncmp(out->scheme, "https", 5) == 0) ? 443 : 80;

    /* Host (and optional :port) */
    const char *slash = strchr(p, '/');
    const char *at    = strchr(p, '@');
    if (at && (!slash || at < slash)) p = at + 1;  /* skip userinfo */

    const char *host_end = slash ? slash : p + strlen(p);
    const char *port_sep = memchr(p, ':', (size_t)(host_end - p));
    if (port_sep) {
        size_t hlen = (size_t)(port_sep - p);
        if (hlen >= sizeof(out->host)) goto host_too_long;
        memcpy(out->host, p, hlen);
        out->port = (uint16_t)atoi(port_sep + 1);
    } else {
        size_t hlen = (size_t)(host_end - p);
        if (hlen >= sizeof(out->host)) goto host_too_long;
        memcpy(out->host, p, hlen);
    }

    /* Path */
    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= sizeof(out->path)) {
            flux_set_error(FLUX_ERR_HTTP_PARSE, "URL path too long");
            return -1;
        }
        memcpy(out->path, slash, plen + 1);
    } else {
        out->path[0] = '/'; out->path[1] = '\0';
    }
    return 0;

host_too_long:
    flux_set_error(FLUX_ERR_HTTP_PARSE, "URL hostname too long");
    return -1;
}

/* ── Request builder ──────────────────────────────────────────────────────── */

static int send_request(flux_conn_t *conn, const char *req, size_t len) {
    ssize_t n = flux_conn_send(conn, req, len);
    if (n < 0) return -1;
    if ((size_t)n != len) {
        flux_set_error(FLUX_ERR_NETWORK, "partial HTTP request send");
        return -1;
    }
    return 0;
}

int flux_http_send_range_request(flux_segment_t *seg) {
    flux_download_t *dl = seg->dl;
    char req[4096];
    int  len;

    if (seg->range_end == UINT64_MAX) {
        /* No upper bound — server will send from start to EOF */
        if (seg->range_start == 0) {
            len = snprintf(req, sizeof(req),
                "GET %s %s\r\n"
                "Host: %s\r\n"
                "User-Agent: " HTTP_USER_AGENT "\r\n"
                "Accept: */*\r\n"
                "Connection: close\r\n"
                "\r\n",
                dl->url_path, HTTP_VERSION, dl->host);
        } else {
            len = snprintf(req, sizeof(req),
                "GET %s %s\r\n"
                "Host: %s\r\n"
                "User-Agent: " HTTP_USER_AGENT "\r\n"
                "Accept: */*\r\n"
                "Range: bytes=%lu-\r\n"
                "Connection: keep-alive\r\n"
                "\r\n",
                dl->url_path, HTTP_VERSION, dl->host,
                (unsigned long)seg->range_start);
        }
    } else {
        len = snprintf(req, sizeof(req),
            "GET %s %s\r\n"
            "Host: %s\r\n"
            "User-Agent: " HTTP_USER_AGENT "\r\n"
            "Accept: */*\r\n"
            "Range: bytes=%lu-%lu\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            dl->url_path, HTTP_VERSION, dl->host,
            (unsigned long)(seg->range_start + seg->written),
            (unsigned long) seg->range_end);
    }

    if (len < 0 || (size_t)len >= sizeof(req)) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "request too long");
        return -1;
    }
    return send_request(seg->conn, req, (size_t)len);
}

int flux_http_send_head(flux_conn_t *conn, const http_url_t *url) {
    char req[2048];
    int  len = snprintf(req, sizeof(req),
        "HEAD %s %s\r\n"
        "Host: %s\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        url->path, HTTP_VERSION, url->host);

    if (len < 0 || (size_t)len >= sizeof(req)) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "HEAD request too long");
        return -1;
    }
    return send_request(conn, req, (size_t)len);
}

/* ── Header parser helpers ────────────────────────────────────────────────── */

static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void rtrim(char *s) {
    size_t len = strlen(s);
    while (len && (s[len-1] == ' ' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

static void parse_one_header(http_response_t *r, char *name, char *value) {
    /* Lowercase the header name for case-insensitive comparison */
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    rtrim(value);
    value = skip_ws(value);

    if (strcmp(name, "content-length") == 0) {
        r->content_length = (uint64_t)strtoull(value, NULL, 10);

    } else if (strcmp(name, "content-range") == 0) {
        /* bytes 0-99/1000 */
        unsigned long long s = 0, e = 0, t = 0;
        if (sscanf(value, "bytes %llu-%llu/%llu", &s, &e, &t) == 3) {
            r->content_range_start = s;
            r->content_range_end   = e;
            r->content_range_total = t;
            if (r->content_length == 0)
                r->content_length = e - s + 1;
        }

    } else if (strcmp(name, "transfer-encoding") == 0) {
        r->chunked = (strstr(value, "chunked") != NULL) ? 1 : 0;

    } else if (strcmp(name, "connection") == 0) {
        r->keepalive = (strstr(value, "keep-alive") != NULL) ? 1 : 0;

    } else if (strcmp(name, "accept-ranges") == 0) {
        r->accept_ranges = (strstr(value, "bytes") != NULL) ? 1 : 0;

    } else if (strcmp(name, "location") == 0) {
        strncpy(r->location, value, sizeof(r->location) - 1);

    } else if (strcmp(name, "content-type") == 0) {
        strncpy(r->content_type, value, sizeof(r->content_type) - 1);

    } else if (strcmp(name, "etag") == 0) {
        strncpy(r->etag, value, sizeof(r->etag) - 1);

    } else if (strcmp(name, "last-modified") == 0) {
        strncpy(r->last_modified, value, sizeof(r->last_modified) - 1);
    }
}

/* Parse headers from a flat char buffer.
 * Returns 1 if the end of headers (\r\n\r\n) was found, 0 if incomplete. */
static int parse_headers_buf(http_response_t *r, char *buf, size_t len) {
    /* Find \r\n\r\n */
    const char *end_marker = "\r\n\r\n";
    char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(buf + i, end_marker, 4) == 0) {
            hdr_end = buf + i + 4;
            r->header_end = i + 4;
            break;
        }
    }
    if (!hdr_end) return 0;   /* incomplete */

    /* Null-terminate the header block for string parsing */
    char tmp = *hdr_end;
    *hdr_end = '\0';

    /* Status line */
    char *line = buf;
    char *nl   = strstr(line, "\r\n");
    if (!nl) { *hdr_end = tmp; return -1; }
    *nl = '\0';
    /* HTTP/1.1 200 OK */
    char *sp1 = strchr(line, ' ');
    if (!sp1) { *hdr_end = tmp; return -1; }
    r->status = atoi(sp1 + 1);
    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2) strncpy(r->reason, sp2 + 1, sizeof(r->reason) - 1);
    rtrim(r->reason);

    line = nl + 2;

    /* Header lines */
    while (*line && line < hdr_end - 2) {
        nl = strstr(line, "\r\n");
        if (!nl) break;
        *nl = '\0';

        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            parse_one_header(r, line, colon + 1);
        }
        line = nl + 2;
    }

    *hdr_end = tmp;
    return 1;
}

/* ── Segment header parser (stateful across multiple EPOLLIN events) ─────── */
int flux_http_parse_headers(flux_segment_t *seg) {
    flux_conn_t     *conn = seg->conn;
    http_response_t *r    = seg->response;

    /* Append buffered data into r->raw */
    size_t avail = flux_conn_buffered(conn);
    if (avail == 0) return 0;

    size_t space = sizeof(r->raw) - r->raw_len;
    if (avail > space) avail = space;
    flux_conn_drain(conn, r->raw + r->raw_len, avail);
    r->raw_len += avail;

    int rc = parse_headers_buf(r, r->raw, r->raw_len);
    if (rc <= 0) return rc;

    /* Validate status */
    if (r->status >= 400) {
        flux_set_error(http_status_to_flux_error(r->status),
                       "HTTP %d %s", r->status, r->reason);
        return -1;
    }
    if (r->status == 301 || r->status == 302 || r->status == 307 || r->status == 308) {
        flux_set_error(FLUX_ERR_HTTP_REDIR, "redirect to: %s", r->location);
        return -1;  /* downloader.c handles redirect logic */
    }

    /* Update segment's keepalive flag from response */
    conn->keepalive = r->keepalive;

    /* Any bytes after the header end are body — push back into ring buffer */
    size_t body_prefix = r->raw_len - r->header_end;
    if (body_prefix > 0) {
        /* Prepend to ring buffer: adjust start pointer back */
        if (conn->rbuf_start >= body_prefix) {
            conn->rbuf_start -= body_prefix;
            memcpy(conn->rbuf + conn->rbuf_start,
                   r->raw + r->header_end, body_prefix);
        }
    }

    seg->state = SEG_BODY;
    return 1;
}

int flux_http_parse_head_response(flux_conn_t *conn, http_response_t *out) {
    memset(out, 0, sizeof(*out));
    /* Collect all buffered data */
    out->raw_len = flux_conn_drain(conn, out->raw, sizeof(out->raw) - 1);
    return parse_headers_buf(out, out->raw, out->raw_len);
}

/* ── Chunked transfer ─────────────────────────────────────────────────────── */
int64_t flux_http_next_chunk_size(flux_conn_t *conn) {
    /* Chunk size line: "<hex digits>\r\n" */
    char line[32];
    size_t i = 0;
    while (i < sizeof(line) - 1) {
        if (flux_conn_buffered(conn) < i + 2) return 0; /* incomplete */
        uint8_t ch;
        flux_conn_peek(conn, line, i + 1);
        ch = (uint8_t)line[i];
        if (ch == '\n') {
            line[i] = '\0';
            if (i > 0 && line[i-1] == '\r') line[i-1] = '\0';
            flux_conn_drain(conn, NULL, i + 1); /* consume the line */
            return (int64_t)strtoll(line, NULL, 16);
        }
        i++;
    }
    flux_set_error(FLUX_ERR_HTTP_PARSE, "chunk size line too long");
    return -1;
}

/* ── Status → error mapping ───────────────────────────────────────────────── */
flux_error_t http_status_to_flux_error(int status) {
    switch (status) {
        case 403: return FLUX_ERR_FORBIDDEN;
        case 404: return FLUX_ERR_NOT_FOUND_;
        case 416: return FLUX_ERR_HTTP_RANGE;
        default:
            if (status >= 500) return FLUX_ERR_SERVER;
            return FLUX_ERR_HTTP_STATUS;
    }
}
