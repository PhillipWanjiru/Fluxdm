/* FluxDM — core/src/protocol/http.c */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include "../../include/protocol/http.h"
#include "../../include/errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>

/* ═════════════════════════════════════════════════════════════════════════════
 *  URLs
 * ═══════════════════════════════════════════════════════════════════════════ */

static int url_err(const char *what, const char *url) {
    flux_set_error(FLUX_ERR_HTTP_PARSE, "%s: %.200s", what, url ? url : "(null)");
    return -1;
}

/* Characters that may not appear raw in a request-target. */
static int needs_pct_encoding(unsigned char c) {
    return c <= 0x20 || c >= 0x7F || strchr("\"<>\\^`{|}", c) != NULL;
}

/* Append src[0..len) to dst, percent-encoding unsafe bytes. Stops at '#'
 * (fragments are never sent). Returns 0, or -1 on overflow. */
static int append_encoded(char *dst, size_t cap, size_t *pos, const char *src, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '#') break;
        if (needs_pct_encoding(c)) {
            if (*pos + 3 >= cap) return -1;
            dst[(*pos)++] = '%';
            dst[(*pos)++] = hex[c >> 4];
            dst[(*pos)++] = hex[c & 0xF];
        } else {
            if (*pos + 1 >= cap) return -1;
            dst[(*pos)++] = (char)c;
        }
    }
    dst[*pos] = '\0';
    return 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode src[0..len) into dst (always NUL-terminated). */
static int pct_decode(char *dst, size_t cap, const char *src, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '%' && i + 2 < len && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            c = (char)((hexval(src[i + 1]) << 4) | hexval(src[i + 2]));
            i += 2;
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

/* ASCII-lowercase a host name in place. DNS names and IP literals are
 * case-insensitive; normalising keeps "same server?" comparisons honest. An
 * IPv6 zone id (after '%') names an interface and is left alone. */
static void lower_host(char *host) {
    if (strchr(host, '%')) return;
    for (; *host; host++) *host = (char)tolower((unsigned char)*host);
}

int http_url_parse(const char *url_in, http_url_t *out) {
    memset(out, 0, sizeof(*out));
    if (!url_in) return url_err("URL is empty", url_in);

    /* Surrounding whitespace is never part of a URL (think: pasted with a newline). */
    while (*url_in == ' ' || *url_in == '\t' || *url_in == '\r' || *url_in == '\n') url_in++;
    size_t ulen = strlen(url_in);
    while (ulen > 0 && (url_in[ulen - 1] == ' ' || url_in[ulen - 1] == '\t' ||
                        url_in[ulen - 1] == '\r' || url_in[ulen - 1] == '\n')) ulen--;
    if (ulen >= FLUX_MAX_URL) return url_err("URL too long", url_in);
    char url_buf[FLUX_MAX_URL];
    memcpy(url_buf, url_in, ulen);
    url_buf[ulen] = '\0';
    const char *url = url_buf;

    const char *colon = strstr(url, "://");
    if (!colon) return url_err("URL missing scheme", url);

    size_t slen = (size_t)(colon - url);
    if (slen == 4 && strncasecmp(url, "http", 4) == 0) {
        memcpy(out->scheme, "http", 5);
        out->port = 80;
    } else if (slen == 5 && strncasecmp(url, "https", 5) == 0) {
        memcpy(out->scheme, "https", 6);
        out->port = 443;
    } else {
        return url_err("unsupported URL scheme (only http and https)", url);
    }

    const char *p        = colon + 3;
    const char *auth_end = p + strcspn(p, "/?#");

    /* userinfo@ — the LAST '@' in the authority separates it from the host */
    const char *at = memrchr(p, '@', (size_t)(auth_end - p));
    if (at) {
        if (pct_decode(out->userinfo, sizeof(out->userinfo), p, (size_t)(at - p)) < 0)
            return url_err("URL credentials too long", url);
        p = at + 1;
    }

    /* host (bracketed IPv6 literal or plain) and optional :port */
    const char *after;
    if (p < auth_end && *p == '[') {
        const char *rb = memchr(p, ']', (size_t)(auth_end - p));
        if (!rb) return url_err("unterminated IPv6 literal in URL", url);
        size_t hlen = (size_t)(rb - (p + 1));
        if (hlen == 0 || hlen >= sizeof(out->host)) return url_err("bad host in URL", url);
        for (size_t i = 0; i < hlen; i++) {
            unsigned char c = (unsigned char)p[1 + i];
            if (!(isxdigit(c) || c == ':' || c == '.' || c == '%' || c == '-' ||
                  isalnum(c) || c == '_'))
                return url_err("bad IPv6 literal in URL", url);
        }
        memcpy(out->host, p + 1, hlen);
        lower_host(out->host);
        after = rb + 1;
    } else {
        const char *pc   = memchr(p, ':', (size_t)(auth_end - p));
        const char *hend = pc ? pc : auth_end;
        size_t      hlen = (size_t)(hend - p);
        if (hlen == 0)                   return url_err("URL has no host", url);
        if (hlen >= sizeof(out->host))   return url_err("URL hostname too long", url);
        for (size_t i = 0; i < hlen; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c <= 0x20 || c >= 0x7F)  return url_err("illegal character in URL host", url);
        }
        memcpy(out->host, p, hlen);
        lower_host(out->host);
        after = hend;
    }

    if (after < auth_end) {
        if (*after != ':') return url_err("garbage after host in URL", url);
        const char *d = after + 1;
        if (d < auth_end) {                      /* "host:" with no digits = default port */
            unsigned long port = 0;
            for (; d < auth_end; d++) {
                if (*d < '0' || *d > '9') return url_err("non-numeric port in URL", url);
                port = port * 10 + (unsigned long)(*d - '0');
                if (port > 65535) return url_err("port out of range in URL", url);
            }
            if (port == 0) return url_err("port out of range in URL", url);
            out->port = (uint16_t)port;
        }
    }

    /* Request-target: path + query, '#fragment' dropped, unsafe bytes encoded */
    size_t pos = 0;
    const char *rest = auth_end;
    if (*rest != '/') out->path[pos++] = '/';
    if (append_encoded(out->path, sizeof(out->path), &pos, rest, strlen(rest)) < 0)
        return url_err("URL path too long", url);
    out->path[pos] = '\0';
    return 0;
}

int http_url_is_tls(const http_url_t *u) {
    return strcmp(u->scheme, "https") == 0;
}

int http_host_header(const http_url_t *u, char *out, size_t cap) {
    int         v6      = strchr(u->host, ':') != NULL;
    uint16_t    def     = http_url_is_tls(u) ? 443 : 80;
    int n;
    if (u->port == def) n = snprintf(out, cap, v6 ? "[%s]" : "%s", u->host);
    else                n = snprintf(out, cap, v6 ? "[%s]:%u" : "%s:%u", u->host, u->port);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* Collapse "." and ".." segments in the path part of `path` (query kept). */
static void remove_dot_segments(char *path) {
    char  query[FLUX_MAX_URL];
    char *q = strchr(path, '?');
    query[0] = '\0';
    if (q) { snprintf(query, sizeof(query), "%s", q); *q = '\0'; }

    char   out[FLUX_MAX_URL];
    size_t o = 0;
    const char *p = path;
    while (*p) {
        const char *seg  = p + 1;                         /* p points at '/' */
        const char *end  = strchr(seg, '/');
        size_t      slen = end ? (size_t)(end - seg) : strlen(seg);

        if (slen == 1 && seg[0] == '.') {
            if (!end) out[o++] = '/';
        } else if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            if (!end) out[o++] = '/';
        } else {
            out[o++] = '/';
            memcpy(out + o, seg, slen);
            o += slen;
        }
        p = end ? end : seg + slen;
    }
    if (o == 0) out[o++] = '/';
    out[o] = '\0';

    size_t olen = strlen(out);
    size_t qlen = strlen(query);
    if (olen + qlen >= FLUX_MAX_URL) qlen = FLUX_MAX_URL - 1 - olen;
    memcpy(path, out, olen);
    memcpy(path + olen, query, qlen);
    path[olen + qlen] = '\0';
}

/* Parse an absolute redirect target. The credentials of `base` are carried
 * over only when the target is the very same origin (scheme, host and port):
 * never to another host, port or a downgraded scheme. */
static int resolve_absolute(const http_url_t *base, const char *abs_url, http_url_t *out) {
    if (http_url_parse(abs_url, out) < 0) return -1;
    if (out->userinfo[0] == '\0' && base->userinfo[0] != '\0' &&
        strcmp(out->scheme, base->scheme) == 0 && strcmp(out->host, base->host) == 0 &&
        out->port == base->port)
        memcpy(out->userinfo, base->userinfo, sizeof(out->userinfo));
    return 0;
}

int http_url_resolve(const http_url_t *base, const char *location, http_url_t *out) {
    char loc[FLUX_MAX_URL];
    if (!location) return url_err("empty redirect location", location);

    while (isspace((unsigned char)*location)) location++;
    size_t llen = strlen(location);
    while (llen > 0 && isspace((unsigned char)location[llen - 1])) llen--;
    if (llen == 0 || llen >= sizeof(loc)) return url_err("bad redirect location", location);
    memcpy(loc, location, llen);
    loc[llen] = '\0';

    /* Absolute http(s) URL */
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0)
        return resolve_absolute(base, loc, out);

    /* Some other scheme ("ftp://…", "data:…") */
    {
        size_t i = 0;
        if (isalpha((unsigned char)loc[0])) {
            while (isalnum((unsigned char)loc[i]) || loc[i] == '+' || loc[i] == '-' || loc[i] == '.') i++;
            if (loc[i] == ':') return url_err("redirect to unsupported scheme", loc);
        }
    }

    /* Scheme-relative: //host/path */
    if (loc[0] == '/' && loc[1] == '/') {
        char abs_url[FLUX_MAX_URL + 8];
        snprintf(abs_url, sizeof(abs_url), "%s:%s", base->scheme, loc);
        return resolve_absolute(base, abs_url, out);
    }

    /* Same origin: only the path/query changes. */
    *out = *base;

    char merged[FLUX_MAX_URL];
    if (loc[0] == '/') {
        snprintf(merged, sizeof(merged), "%s", loc);
    } else if (loc[0] == '?') {
        char *q = strchr(base->path, '?');
        size_t plen = q ? (size_t)(q - base->path) : strlen(base->path);
        if (plen + strlen(loc) >= sizeof(merged)) return url_err("redirect location too long", loc);
        memcpy(merged, base->path, plen);
        merged[plen] = '\0';
        strcat(merged, loc);
    } else if (loc[0] == '#') {
        snprintf(merged, sizeof(merged), "%s", base->path);
    } else {
        char *q = strchr(base->path, '?');
        size_t plen = q ? (size_t)(q - base->path) : strlen(base->path);
        while (plen > 0 && base->path[plen - 1] != '/') plen--;     /* keep the directory */
        if (plen + strlen(loc) >= sizeof(merged)) return url_err("redirect location too long", loc);
        memcpy(merged, base->path, plen);
        merged[plen] = '\0';
        strcat(merged, loc);
    }

    size_t pos = 0;
    out->path[0] = '\0';
    if (append_encoded(out->path, sizeof(out->path), &pos, merged, strlen(merged)) < 0)
        return url_err("redirect location too long", loc);
    remove_dot_segments(out->path);
    return 0;
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  Requests
 * ═══════════════════════════════════════════════════════════════════════════ */

static size_t base64_encode(const char *in, size_t len, char *out, size_t cap) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)(unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)(unsigned char)in[i + 2];
        if (o + 4 >= cap) return 0;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

int http_build_get(char *buf, size_t cap, const http_url_t *u,
                   int ranged, uint64_t first, uint64_t last) {
    char host[320];
    if (http_host_header(u, host, sizeof(host)) < 0) return -1;

    char auth[512] = "";
    if (u->userinfo[0]) {
        char b64[400];
        if (base64_encode(u->userinfo, strlen(u->userinfo), b64, sizeof(b64)) == 0) return -1;
        snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", b64);
    }

    char range[96] = "";
    if (ranged) {
        if (last == UINT64_MAX)
            snprintf(range, sizeof(range), "Range: bytes=%llu-\r\n", (unsigned long long)first);
        else
            snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
                     (unsigned long long)first, (unsigned long long)last);
    }

    int n = snprintf(buf, cap,
        "GET %s " HTTP_VERSION "\r\n"
        "Host: %s\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "%s%s"
        "Connection: close\r\n"
        "\r\n",
        u->path, host, auth, range);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* ═════════════════════════════════════════════════════════════════════════════
 *  Responses
 * ═══════════════════════════════════════════════════════════════════════════ */

static void copy_trunc(char *dst, size_t cap, const char *src, size_t len) {
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int name_is(const char *name, size_t nlen, const char *lit) {
    return nlen == strlen(lit) && strncasecmp(name, lit, nlen) == 0;
}

/* Parse an unsigned decimal; the whole string must be digits. */
static int parse_u64(const char *s, size_t len, uint64_t *out) {
    if (len == 0) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return -1;       /* overflow */
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* Does the comma-separated header list contain `tok` (case-insensitive,
 * parameters after ';' ignored)? */
static int list_has_token(const char *v, size_t len, const char *tok) {
    size_t tlen = strlen(tok);
    size_t i = 0;
    while (i < len) {
        while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
        size_t s = i;
        while (i < len && v[i] != ',' && v[i] != ';') i++;
        size_t e = i;
        while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
        if (e - s == tlen && strncasecmp(v + s, tok, tlen) == 0) return 1;
        while (i < len && v[i] != ',') i++;               /* skip parameters */
    }
    return 0;
}

/* Content-Range forms: S-E/T (known total), S-E/? (unknown total), ?/T (unsatisfied, 416)
 * where ? is the literal asterisk. */
static int parse_content_range(http_response_t *r, const char *v, size_t len) {
    if (len < 7 || strncasecmp(v, "bytes", 5) != 0) return -1;
    size_t i = 5;
    while (i < len && (v[i] == ' ' || v[i] == '\t')) i++;

    if (i < len && v[i] == '*') {                          /* unsatisfied: asterisk/N */
        i++;
        if (i >= len || v[i] != '/') return -1;
        uint64_t total;
        if (parse_u64(v + i + 1, len - i - 1, &total) < 0) return -1;
        r->content_range_unsat = 1;
        r->content_range_total = total;
        r->has_content_range   = 1;
        return 0;
    }

    size_t d1 = i;
    while (i < len && v[i] >= '0' && v[i] <= '9') i++;
    uint64_t s, e;
    if (parse_u64(v + d1, i - d1, &s) < 0 || i >= len || v[i] != '-') return -1;
    size_t d2 = ++i;
    while (i < len && v[i] >= '0' && v[i] <= '9') i++;
    if (parse_u64(v + d2, i - d2, &e) < 0 || i >= len || v[i] != '/' || e < s) return -1;
    i++;
    uint64_t total = 0;
    if (i < len && v[i] == '*') total = 0;
    else if (parse_u64(v + i, len - i, &total) < 0) return -1;

    r->content_range_start = s;
    r->content_range_end   = e;
    r->content_range_total = total;
    r->has_content_range   = 1;
    return 0;
}

int http_parse_head(http_response_t *r, const char *buf, size_t len) {
    memset(r, 0, sizeof(*r));

    /* Locate the blank line that ends the head ("\r\n\r\n", or bare "\n\n"). */
    size_t end = 0;
    for (size_t i = 0; i < len && i < HTTP_MAX_HEADER_LEN; i++) {
        if (buf[i] != '\n') continue;
        if (i + 1 < len && buf[i + 1] == '\n')                     { end = i + 2; break; }
        if (i + 2 < len && buf[i + 1] == '\r' && buf[i + 2] == '\n') { end = i + 3; break; }
    }
    if (end == 0) {
        if (len >= HTTP_MAX_HEADER_LEN) {
            flux_set_error(FLUX_ERR_HTTP_PARSE, "HTTP response head exceeds %d bytes",
                           HTTP_MAX_HEADER_LEN);
            return -1;
        }
        return 0;                                                  /* need more data */
    }

    /* ── Status line: HTTP/1.x SP 3DIGIT [SP reason] ─────────────────────── */
    const char *p     = buf;
    const char *limit = buf + end;
    const char *eol   = memchr(p, '\n', (size_t)(limit - p));
    size_t      ll    = (size_t)(eol - p);
    if (ll > 0 && p[ll - 1] == '\r') ll--;

    if (ll < 12 || strncmp(p, "HTTP/1.", 7) != 0 || (p[7] != '0' && p[7] != '1') ||
        p[8] != ' ' || !isdigit((unsigned char)p[9]) || !isdigit((unsigned char)p[10]) ||
        !isdigit((unsigned char)p[11])) {
        flux_set_error(FLUX_ERR_HTTP_PARSE, "malformed HTTP status line");
        return -1;
    }
    r->version_minor = (uint8_t)(p[7] - '0');
    r->status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
    if ((ll > 12 && p[12] != ' ') || r->status < 100 || r->status > 599) {   /* exactly three digits */
        flux_set_error(FLUX_ERR_HTTP_PARSE, "malformed HTTP status code");
        return -1;
    }
    if (ll > 13) copy_trunc(r->reason, sizeof(r->reason), p + 13, ll - 13);

    /* ── Header fields ────────────────────────────────────────────────────── */
    int conn_close = 0, conn_keepalive = 0;
    p = eol + 1;
    while (p < limit) {
        eol = memchr(p, '\n', (size_t)(limit - p));
        if (!eol) break;
        ll = (size_t)(eol - p);
        if (ll > 0 && p[ll - 1] == '\r') ll--;
        if (ll == 0) break;                                        /* blank line: end of head */

        const char *colon = memchr(p, ':', ll);
        if (colon) {
            const char *name = p;
            size_t      nlen = (size_t)(colon - p);
            while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) nlen--;

            const char *v    = colon + 1;
            size_t      vlen = ll - (size_t)(v - p);
            while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
            while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) vlen--;

            if (name_is(name, nlen, "content-length")) {
                size_t first = 0;
                while (first < vlen && v[first] != ',') first++;   /* "5, 5" from proxies */
                uint64_t cl;
                if (parse_u64(v, first, &cl) < 0) {
                    flux_set_error(FLUX_ERR_HTTP_PARSE, "invalid Content-Length");
                    return -1;
                }
                if (r->has_content_length && r->content_length != cl) {
                    flux_set_error(FLUX_ERR_HTTP_PARSE, "conflicting Content-Length headers");
                    return -1;
                }
                r->has_content_length = 1;
                r->content_length     = cl;
            } else if (name_is(name, nlen, "content-range")) {
                if (parse_content_range(r, v, vlen) < 0) {
                    flux_set_error(FLUX_ERR_HTTP_PARSE, "invalid Content-Range");
                    return -1;
                }
            } else if (name_is(name, nlen, "transfer-encoding")) {
                if (list_has_token(v, vlen, "chunked")) r->chunked = 1;
            } else if (name_is(name, nlen, "connection")) {
                if (list_has_token(v, vlen, "close"))      conn_close = 1;
                if (list_has_token(v, vlen, "keep-alive")) conn_keepalive = 1;
            } else if (name_is(name, nlen, "accept-ranges")) {
                r->accept_ranges = list_has_token(v, vlen, "bytes") ? 1 : 0;
            } else if (name_is(name, nlen, "retry-after")) {
                uint64_t s;
                if (parse_u64(v, vlen, &s) == 0) r->retry_after_s = s > 3600 ? 3600 : (uint32_t)s;
            } else if (name_is(name, nlen, "location")) {
                copy_trunc(r->location, sizeof(r->location), v, vlen);
            } else if (name_is(name, nlen, "content-type")) {
                copy_trunc(r->content_type, sizeof(r->content_type), v, vlen);
            } else if (name_is(name, nlen, "etag")) {
                copy_trunc(r->etag, sizeof(r->etag), v, vlen);
            } else if (name_is(name, nlen, "last-modified")) {
                copy_trunc(r->last_modified, sizeof(r->last_modified), v, vlen);
            }
        }
        p = eol + 1;
    }

    r->keepalive = (r->version_minor >= 1) ? (uint8_t)!conn_close : (uint8_t)conn_keepalive;
    return (int)end;
}

int flux_http_read_head(flux_conn_t *conn, http_response_t *out) {
    for (;;) {
        size_t avail = flux_conn_buffered(conn);
        if (avail == 0) { memset(out, 0, sizeof(*out)); return 0; }

        int n = http_parse_head(out, (const char *)(conn->rbuf + conn->rbuf_start), avail);
        if (n <= 0) return n;                           /* 0 = incomplete, -1 = error */
        flux_conn_drain(conn, NULL, (size_t)n);         /* consume the head only */

        /* Interim responses (100 Continue, 103 Early Hints, ...) precede the real
         * one on the same connection: skip them. 101 cannot be answered to a GET
         * and is handed up as an error. */
        if (out->status >= 100 && out->status < 200 && out->status != 101) continue;
        return 1;
    }
}

int flux_http_parse_head_response(flux_conn_t *conn, http_response_t *out) {
    return flux_http_read_head(conn, out);
}

/* ── Status → error mapping ───────────────────────────────────────────────── */
flux_error_t http_status_to_flux_error(int status) {
    switch (status) {
        case 401:
        case 403: return FLUX_ERR_FORBIDDEN;
        case 404:
        case 410: return FLUX_ERR_NOT_FOUND_;
        case 416: return FLUX_ERR_HTTP_RANGE;
        default:
            if (status >= 500) return FLUX_ERR_SERVER;
            return FLUX_ERR_HTTP_STATUS;
    }
}

int http_status_is_retryable(int status) {
    return status == 408 || status == 425 || status == 429 ||
           status == 500 || status == 502 || status == 503 || status == 504;
}
