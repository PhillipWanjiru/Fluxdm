/* FluxDM — tests/test_unit_io.c
 * Unit tests for the byte-level layers of the core: URL handling, request
 * building, response-head parsing, the IPC frame reader / encoder and the choice
 * of the default socket path. None of these touch the network.
 *
 * Build:  cd tests && make unit-io
 * Run:    ./test_unit_io
 */

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/include/errors.h"
#include "../core/include/connection.h"
#include "../core/include/protocol/http.h"
#include "../core/src/ipc/frame_io.h"
#include "../core/src/ipc/sock_path.h"
#include "../ipc/protocol.h"
#include "../ipc/messages.h"

/* ── Tiny test framework (same look as test_unit.c) ───────────────────────── */
static int g_pass = 0, g_fail = 0;

#define RESET  "\033[0m"
#define GREEN  "\033[92m"
#define RED    "\033[91m"
#define CYAN   "\033[96m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"

#define SUITE(name) printf("\n%s%s── %s%s\n", BOLD, CYAN, name, RESET)

#define EXPECT(expr) \
    do { \
        if (expr) { printf("  %s✔%s  %s\n", GREEN, RESET, #expr); g_pass++; } \
        else { printf("  %s✘%s  %s  %s(%s:%d)%s\n", RED, RESET, #expr, DIM, __FILE__, __LINE__, RESET); g_fail++; } \
    } while (0)

#define EXPECT_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a == _b) { printf("  %s✔%s  %s == %s\n", GREEN, RESET, #a, #b); g_pass++; } \
        else { printf("  %s✘%s  %s == %s  (got %lld, expected %lld)  %s(%s:%d)%s\n", \
                      RED, RESET, #a, #b, _a, _b, DIM, __FILE__, __LINE__, RESET); g_fail++; } \
    } while (0)

#define EXPECT_STR(a, b) \
    do { \
        const char *_sa = (a), *_sb = (b); \
        if (strcmp(_sa, _sb) == 0) { printf("  %s✔%s  \"%s\"\n", GREEN, RESET, _sa); g_pass++; } \
        else { printf("  %s✘%s  expected \"%s\", got \"%s\"  %s(%s:%d)%s\n", \
                      RED, RESET, _sb, _sa, DIM, __FILE__, __LINE__, RESET); g_fail++; } \
    } while (0)

/* ── URL parsing ──────────────────────────────────────────────────────────── */
static void test_url_parse(void) {
    SUITE("http_url_parse");
    http_url_t u;

    EXPECT_EQ(http_url_parse("HTTP://Example.com/A", &u), 0);
    EXPECT_STR(u.scheme, "http");
    EXPECT_EQ(u.port, 80);
    EXPECT_STR(u.path, "/A");

    EXPECT_EQ(http_url_parse("http://[::1]:8080/a", &u), 0);
    EXPECT_STR(u.host, "::1");
    EXPECT_EQ(u.port, 8080);

    EXPECT_EQ(http_url_parse("https://[2001:db8::1]/a", &u), 0);
    EXPECT_STR(u.host, "2001:db8::1");
    EXPECT_EQ(u.port, 443);

    EXPECT_EQ(http_url_parse("http://h/a b", &u), 0);
    EXPECT_STR(u.path, "/a%20b");

    EXPECT_EQ(http_url_parse("http://h/a%20b%2Fc", &u), 0);
    EXPECT_STR(u.path, "/a%20b%2Fc");

    EXPECT_EQ(http_url_parse("http://h/caf\xC3\xA9", &u), 0);
    EXPECT_STR(u.path, "/caf%C3%A9");

    EXPECT_EQ(http_url_parse("http://h/p?x=1&y=a b#frag", &u), 0);
    EXPECT_STR(u.path, "/p?x=1&y=a%20b");

    EXPECT_EQ(http_url_parse("http://h?x=1", &u), 0);
    EXPECT_STR(u.path, "/?x=1");

    EXPECT_EQ(http_url_parse("http://user:p%40ss@h:81/f", &u), 0);
    EXPECT_STR(u.host, "h");
    EXPECT_EQ(u.port, 81);
    EXPECT(strncmp(u.userinfo, "user:", 5) == 0);

    EXPECT_EQ(http_url_parse("http://h:65535/", &u), 0);
    EXPECT_EQ(u.port, 65535);

    EXPECT_EQ(http_url_parse("http://h:65536/", &u), -1);
    EXPECT_EQ(http_url_parse("http://h:abc/", &u), -1);
    EXPECT_EQ(http_url_parse("http:///nohost", &u), -1);
    EXPECT_EQ(http_url_parse("ftp://h/x", &u), -1);
    EXPECT_EQ(http_url_parse("", &u), -1);
    EXPECT_EQ(http_url_parse("http://[::1/x", &u), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_HTTP_PARSE);

    /* oversize input must be rejected, not truncated into something else */
    char *big = malloc(FLUX_MAX_URL + 64);
    memcpy(big, "http://h/", 9);
    memset(big + 9, 'a', FLUX_MAX_URL + 40);
    big[9 + FLUX_MAX_URL + 40] = '\0';
    EXPECT_EQ(http_url_parse(big, &u), -1);
    free(big);

    /* a host name longer than the field */
    char longhost[400];
    strcpy(longhost, "http://");
    memset(longhost + 7, 'h', 300);
    strcpy(longhost + 307, "/x");
    EXPECT_EQ(http_url_parse(longhost, &u), -1);
}

static void test_url_resolve(void) {
    SUITE("http_url_resolve");
    http_url_t base, u;
    EXPECT_EQ(http_url_parse("http://u:p@host:8080/a/b/c?old=1", &base), 0);

    EXPECT_EQ(http_url_resolve(&base, "http://other.example/x", &u), 0);
    EXPECT_STR(u.host, "other.example");
    EXPECT_STR(u.path, "/x");
    EXPECT_STR(u.userinfo, "");                       /* credentials never follow a host change */

    EXPECT_EQ(http_url_resolve(&base, "//cdn.example.com/y", &u), 0);
    EXPECT_STR(u.scheme, "http");
    EXPECT_STR(u.host, "cdn.example.com");
    EXPECT_STR(u.path, "/y");

    EXPECT_EQ(http_url_resolve(&base, "/z?q=1", &u), 0);
    EXPECT_STR(u.host, "host");
    EXPECT_EQ(u.port, 8080);
    EXPECT_STR(u.path, "/z?q=1");
    EXPECT(strncmp(u.userinfo, "u:p", 3) == 0);       /* same host keeps them */

    EXPECT_EQ(http_url_resolve(&base, "sub/file", &u), 0);
    EXPECT_STR(u.path, "/a/b/sub/file");
    EXPECT_EQ(http_url_resolve(&base, "../up", &u), 0);
    EXPECT_STR(u.path, "/a/up");
    EXPECT_EQ(http_url_resolve(&base, "./x", &u), 0);
    EXPECT_STR(u.path, "/a/b/x");
    EXPECT_EQ(http_url_resolve(&base, "../../../../way/out", &u), 0);
    EXPECT_STR(u.path, "/way/out");                   /* cannot climb above the root */
    EXPECT_EQ(http_url_resolve(&base, "?k=v", &u), 0);
    EXPECT_STR(u.path, "/a/b/c?k=v");
    EXPECT_EQ(http_url_resolve(&base, "/p#frag", &u), 0);
    EXPECT_STR(u.path, "/p");
    EXPECT_EQ(http_url_resolve(&base, "/with space", &u), 0);
    EXPECT_STR(u.path, "/with%20space");

    EXPECT_EQ(http_url_resolve(&base, "https://secure.example/x", &u), 0);
    EXPECT_STR(u.scheme, "https");
    EXPECT_EQ(u.port, 443);
    EXPECT_EQ(http_url_resolve(&base, "ftp://nope/x", &u), -1);
    EXPECT_EQ(http_url_resolve(&base, "", &u), -1);
}

static void test_host_header(void) {
    SUITE("http_host_header / http_url_is_tls");
    http_url_t u;
    char h[300];

    http_url_parse("http://h/", &u);                 http_host_header(&u, h, sizeof h); EXPECT_STR(h, "h");
    http_url_parse("http://h:8080/", &u);            http_host_header(&u, h, sizeof h); EXPECT_STR(h, "h:8080");
    http_url_parse("https://h:443/", &u);            http_host_header(&u, h, sizeof h); EXPECT_STR(h, "h");
    http_url_parse("https://h:80/", &u);             http_host_header(&u, h, sizeof h); EXPECT_STR(h, "h:80");
    http_url_parse("http://[::1]:80/", &u);          http_host_header(&u, h, sizeof h); EXPECT_STR(h, "[::1]");
    http_url_parse("http://[::1]:81/", &u);          http_host_header(&u, h, sizeof h); EXPECT_STR(h, "[::1]:81");
    EXPECT_EQ(http_host_header(&u, h, 3), -1);       /* does not fit */
    http_url_parse("https://h/", &u);                EXPECT_EQ(http_url_is_tls(&u), 1);
    http_url_parse("http://h/", &u);                 EXPECT_EQ(http_url_is_tls(&u), 0);
}

/* ── Request builder ──────────────────────────────────────────────────────── */
static void test_build_get(void) {
    SUITE("http_build_get");
    http_url_t u;
    char buf[2048];

    http_url_parse("http://example.com:8080/dir/file.bin?x=1", &u);
    int n = http_build_get(buf, sizeof buf, &u, 0, 0, 0);
    EXPECT(n > 0);
    EXPECT_EQ((size_t)n, strlen(buf));
    EXPECT(strncmp(buf, "GET /dir/file.bin?x=1 HTTP/1.1\r\n", 32) == 0);
    EXPECT(strstr(buf, "\r\nHost: example.com:8080\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nAccept-Encoding: identity\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nConnection: close\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nUser-Agent: FluxDM/" FLUXDM_VERSION " (+https://github.com/PhillipWanjiru/Fluxdm)\r\n") != NULL);
    EXPECT(strstr(buf, "Range:") == NULL);
    EXPECT(strstr(buf, "Authorization:") == NULL);
    EXPECT(n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0);

    n = http_build_get(buf, sizeof buf, &u, 1, 0, 0);
    EXPECT(strstr(buf, "\r\nRange: bytes=0-0\r\n") != NULL);
    n = http_build_get(buf, sizeof buf, &u, 1, 1048576, 2097151);
    EXPECT(strstr(buf, "\r\nRange: bytes=1048576-2097151\r\n") != NULL);
    n = http_build_get(buf, sizeof buf, &u, 1, 12345, UINT64_MAX);
    EXPECT(strstr(buf, "\r\nRange: bytes=12345-\r\n") != NULL);
    n = http_build_get(buf, sizeof buf, &u, 1, 5000000000ULL, 9999999999ULL);
    EXPECT(strstr(buf, "bytes=5000000000-9999999999") != NULL);   /* beyond 32 bits */

    /* user:pass@ -> Basic (base64 of "user:p@ss w0rd" = dXNlcjpwQHNzIHcwcmQ=) */
    http_url_parse("http://user:p%40ss%20w0rd@example.com/f", &u);
    n = http_build_get(buf, sizeof buf, &u, 0, 0, 0);
    EXPECT(strstr(buf, "\r\nAuthorization: Basic dXNlcjpwQHNzIHcwcmQ=\r\n") != NULL);
    EXPECT(strstr(buf, "p%40ss") == NULL && strstr(buf, "w0rd\r\n") == NULL);   /* never in clear */

    http_url_parse("http://example.com/f", &u);
    EXPECT_EQ(http_build_get(buf, 20, &u, 0, 0, 0), -1);              /* does not fit */
    EXPECT(http_build_get(buf, sizeof buf, &u, 0, 0, 0) > 0);

    http_url_parse("http://[::1]:8080/f", &u);
    n = http_build_get(buf, sizeof buf, &u, 0, 0, 0);
    EXPECT(strstr(buf, "\r\nHost: [::1]:8080\r\n") != NULL);
}

/* ── Response head parser ─────────────────────────────────────────────────── */
static int parse(http_response_t *r, const char *s) {
    return http_parse_head(r, s, strlen(s));
}

static void test_parse_head(void) {
    SUITE("http_parse_head");
    http_response_t r;
    const char *ok =
        "HTTP/1.1 206 Partial Content\r\n"
        "content-length: 100\r\n"
        "CONTENT-RANGE: bytes 10-109/5000\r\n"
        "Accept-Ranges:   bytes  \r\n"
        "ETag: W/\"x\"\r\n"
        "Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
        "Content-Type: application/zip\r\n"
        "\r\n";
    int n = parse(&r, ok);
    EXPECT_EQ((size_t)n, strlen(ok));
    EXPECT_EQ(r.status, 206);
    EXPECT_STR(r.reason, "Partial Content");
    EXPECT_EQ(r.version_minor, 1);
    EXPECT_EQ(r.has_content_length, 1);
    EXPECT_EQ(r.content_length, 100);
    EXPECT_EQ(r.has_content_range, 1);
    EXPECT_EQ(r.content_range_start, 10);
    EXPECT_EQ(r.content_range_end, 109);
    EXPECT_EQ(r.content_range_total, 5000);
    EXPECT_EQ(r.accept_ranges, 1);
    EXPECT_STR(r.etag, "W/\"x\"");
    EXPECT_STR(r.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT");
    EXPECT_STR(r.content_type, "application/zip");
    EXPECT_EQ(r.keepalive, 1);                       /* HTTP/1.1 default */

    /* body bytes after the blank line are not part of the head */
    char withbody[256];
    snprintf(withbody, sizeof withbody, "%sBODYBODY", ok);
    EXPECT_EQ((size_t)http_parse_head(&r, withbody, strlen(withbody)), strlen(ok));

    /* incomplete: no blank line yet */
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"), 0);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r"), 0);
    EXPECT_EQ(parse(&r, "HTTP/1.1 2"), 0);
    EXPECT_EQ(http_parse_head(&r, "", 0), 0);

    /* every prefix of a valid head is "incomplete", never an error or a crash */
    int prefixes_ok = 1;
    for (size_t i = 0; i < strlen(ok); i++)
        if (http_parse_head(&r, ok, i) != 0) { prefixes_ok = 0; break; }
    EXPECT(prefixes_ok);

    /* unsatisfied range (416) and unknown total */
    EXPECT(parse(&r, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */1234\r\n\r\n") > 0);
    EXPECT_EQ(r.status, 416);
    EXPECT_EQ(r.content_range_unsat, 1);
    EXPECT_EQ(r.content_range_total, 1234);
    EXPECT(parse(&r, "HTTP/1.1 206 P\r\nContent-Range: bytes 0-0/*\r\n\r\n") > 0);
    EXPECT_EQ(r.content_range_total, 0);

    /* framing */
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") > 0);
    EXPECT_EQ(r.chunked, 1);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\ntransfer-encoding: gzip, Chunked\r\n\r\n") > 0);
    EXPECT_EQ(r.chunked, 1);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") > 0);
    EXPECT_EQ(r.has_content_length, 1);
    EXPECT_EQ(r.content_length, 0);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\n\r\n") > 0);
    EXPECT_EQ(r.has_content_length, 0);

    /* connection persistence */
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n") > 0);
    EXPECT_EQ(r.keepalive, 0);
    EXPECT(parse(&r, "HTTP/1.0 200 OK\r\n\r\n") > 0);
    EXPECT_EQ(r.version_minor, 0);
    EXPECT_EQ(r.keepalive, 0);
    EXPECT(parse(&r, "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\n\r\n") > 0);
    EXPECT_EQ(r.keepalive, 1);

    /* redirects and retry hints */
    EXPECT(parse(&r, "HTTP/1.1 302 Found\r\nLocation: https://x.example/y?z=1\r\nRetry-After: 7\r\n\r\n") > 0);
    EXPECT_STR(r.location, "https://x.example/y?z=1");
    EXPECT_EQ(r.retry_after_s, 7);

    /* status line without reason phrase is legal */
    EXPECT(parse(&r, "HTTP/1.1 204\r\n\r\n") > 0);
    EXPECT_EQ(r.status, 204);

    /* garbage */
    EXPECT_EQ(parse(&r, "GARBAGE\r\n\r\n"), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_HTTP_PARSE);
    EXPECT_EQ(parse(&r, "HTTP/1.1 abc OK\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 99 Short\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/2.0 200 OK\r\n\r\n"), -1);

    /* invalid Content-Length values are refused outright */
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 7x\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999999\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"), -1);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n") > 0);
    EXPECT_EQ(r.content_length, 5);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length:  7  \r\n\r\n") > 0);
    EXPECT_EQ(r.content_length, 7);

    /* line folding, odd lines, bare LF: tolerated, never overflow */
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nX-Folded: a\r\n  continued\r\nContent-Length: 3\r\n\r\n") > 0);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nNo-Colon-Here\r\nContent-Length: 3\r\n\r\n") != 0);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\nContent-Length: 3\n\n") != 0);

    /* values longer than their fields are cut, not overflowed (ASan watches) */
    size_t cap = 15000;
    char *huge = malloc(cap);
    int pos = snprintf(huge, cap, "HTTP/1.1 302 Found\r\nLocation: http://h/");
    memset(huge + pos, 'x', 9000);
    pos += 9000;
    pos += snprintf(huge + pos, cap - (size_t)pos, "\r\nETag: ");
    memset(huge + pos, 'e', 3000);
    pos += 3000;
    pos += snprintf(huge + pos, cap - (size_t)pos, "\r\n\r\n");
    int hn = http_parse_head(&r, huge, (size_t)pos);
    EXPECT(hn == -1 || hn == pos);
    EXPECT(strlen(r.location) < FLUX_MAX_URL);
    EXPECT(strlen(r.etag) < sizeof r.etag);

    /* a head that never ends is refused once it exceeds the limit */
    char *endless = malloc(HTTP_MAX_HEADER_LEN + 100);
    memcpy(endless, "HTTP/1.1 200 OK\r\nX: ", 20);
    memset(endless + 20, 'y', HTTP_MAX_HEADER_LEN + 60);
    EXPECT_EQ(http_parse_head(&r, endless, HTTP_MAX_HEADER_LEN + 80), -1);
    free(endless);
    free(huge);

    EXPECT_EQ(http_status_is_retryable(503), 1);
    EXPECT_EQ(http_status_is_retryable(429), 1);
    EXPECT_EQ(http_status_is_pushback(429), 1);
    EXPECT_EQ(http_status_is_pushback(503), 1);
    EXPECT_EQ(http_status_is_pushback(500) + http_status_is_pushback(502) + http_status_is_pushback(504) +
              http_status_is_pushback(408) + http_status_is_pushback(404) + http_status_is_pushback(200), 0);
    EXPECT_EQ(http_status_is_retryable(404), 0);
    EXPECT_EQ(http_status_to_flux_error(401), FLUX_ERR_FORBIDDEN);
    EXPECT_EQ(http_status_to_flux_error(410), FLUX_ERR_NOT_FOUND_);
    EXPECT_EQ(http_status_to_flux_error(502), FLUX_ERR_SERVER);
}

/* ── Head parser over the connection buffer ───────────────────────────────── */
static void conn_init(flux_conn_t *c) {
    memset(c, 0, sizeof *c);
    c->rbuf_cap = 64 * 1024;
    c->rbuf = malloc(c->rbuf_cap);
}
static void conn_feed(flux_conn_t *c, const char *s) {
    size_t n = strlen(s);
    memcpy(c->rbuf + c->rbuf_end, s, n);
    c->rbuf_end += n;
}

static void test_read_head(void) {
    SUITE("flux_http_read_head");
    flux_conn_t c;
    http_response_t r;
    char body[16];

    /* interim responses are skipped, the real head is returned, the body stays */
    conn_init(&c);
    conn_feed(&c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n"
                  "HTTP/1.1 206 Partial Content\r\nContent-Length: 4\r\nContent-Range: bytes 0-3/9\r\n\r\nDATA");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 206);
    EXPECT_EQ(r.content_range_total, 9);
    EXPECT_EQ(flux_conn_buffered(&c), 4);
    EXPECT_EQ(flux_conn_drain(&c, body, sizeof body), 4);
    EXPECT(memcmp(body, "DATA", 4) == 0);
    free(c.rbuf);

    /* 101 is a protocol switch, not an interim response */
    conn_init(&c);
    conn_feed(&c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 101);
    free(c.rbuf);

    /* a head arriving in pieces: nothing is consumed until it is complete */
    conn_init(&c);
    conn_feed(&c, "HTTP/1.1 200 OK\r\nContent-Le");
    EXPECT_EQ(flux_http_read_head(&c, &r), 0);
    EXPECT_EQ(flux_conn_buffered(&c), strlen("HTTP/1.1 200 OK\r\nContent-Le"));
    conn_feed(&c, "ngth: 3\r\n\r");
    EXPECT_EQ(flux_http_read_head(&c, &r), 0);
    conn_feed(&c, "\nabc");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.content_length, 3);
    EXPECT_EQ(flux_conn_buffered(&c), 3);
    free(c.rbuf);

    /* 1xx followed by an incomplete head: wait, and keep the interim consumed or not — either way
     * the final result must be right once everything has arrived */
    conn_init(&c);
    conn_feed(&c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-");
    EXPECT_EQ(flux_http_read_head(&c, &r), 0);
    conn_feed(&c, "Length: 1\r\n\r\nZ");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(flux_conn_buffered(&c), 1);
    free(c.rbuf);

    /* malformed */
    conn_init(&c);
    conn_feed(&c, "NOT HTTP AT ALL\r\n\r\n");
    EXPECT_EQ(flux_http_read_head(&c, &r), -1);
    free(c.rbuf);

    /* a head that starts part-way into the buffer (earlier bytes already consumed) */
    conn_init(&c);
    c.rbuf_start = c.rbuf_end = 30000;
    conn_feed(&c, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.content_length, 2);
    EXPECT_EQ(flux_conn_buffered(&c), 2);
    free(c.rbuf);
}

/* ── IPC frames ───────────────────────────────────────────────────────────── */
static void test_frame_encode(void) {
    SUITE("flux_frame_encode");
    uint8_t buf[256];
    const uint8_t pl[5] = {1, 2, 3, 4, 5};

    size_t n = flux_frame_encode(buf, sizeof buf, FLUX_RSP_PROGRESS, FLUX_FLAG_EVENT, 0xA1B2C3D4u, pl, 5);
    EXPECT_EQ(n, 16 + 5);
    flux_header_t h;
    memcpy(&h, buf, sizeof h);
    EXPECT_EQ(h.magic, FLUX_MAGIC);
    EXPECT_EQ(h.version, FLUX_PROTO_VER);
    EXPECT_EQ(h.type, FLUX_RSP_PROGRESS);
    EXPECT_EQ(h.flags, FLUX_FLAG_EVENT);
    EXPECT_EQ(h.id, 0xA1B2C3D4u);
    EXPECT_EQ(h.payload_len, 5);
    EXPECT(memcmp(buf + 16, pl, 5) == 0);

    /* byte layout is the documented little-endian wire format */
    EXPECT(buf[0] == 'X' && buf[1] == 'U' && buf[2] == 'L' && buf[3] == 'F');
    EXPECT(buf[4] == FLUX_PROTO_VER && buf[5] == FLUX_RSP_PROGRESS);
    EXPECT(buf[6] == 0x01 && buf[7] == 0x00);
    EXPECT(buf[8] == 0xD4 && buf[11] == 0xA1);

    EXPECT_EQ(flux_frame_encode(buf, 20, FLUX_RSP_OK, 0, 1, pl, 5), 0);              /* does not fit */
    EXPECT_EQ(flux_frame_encode(buf, sizeof buf, FLUX_RSP_OK, 0, 1, NULL, 0), 16);   /* empty payload */
    EXPECT_EQ(flux_frame_encode(buf, sizeof buf, FLUX_RSP_OK, 0, 1, pl, FLUX_MAX_PAYLOAD + 1), 0);
}

static size_t make_frames(uint8_t *out, size_t cap, int count) {
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        uint8_t pl[40];
        memset(pl, 0x30 + i, sizeof pl);
        off += flux_frame_encode(out + off, cap - off, (flux_msg_type_t)(0x10 + (i % 5)),
                                 (uint16_t)(i & 1), (uint32_t)(1000 + i), pl, (uint32_t)(i % 41));
    }
    return off;
}

static void test_frame_reader(void) {
    SUITE("flux_frame_reader");
    uint8_t stream[8192];
    size_t total = make_frames(stream, sizeof stream, 25);
    flux_frame_reader_t rd;
    flux_header_t h;
    const uint8_t *pl;

    /* everything at once */
    flux_frame_reader_init(&rd);
    EXPECT_EQ(flux_frame_reader_feed(&rd, stream, total), 0);
    int got = 0, ordered = 1, intact = 1;
    while (flux_frame_reader_next(&rd, &h, &pl) == FRAME_READY) {
        if (h.id != (uint32_t)(1000 + got)) ordered = 0;
        if (h.payload_len != (uint32_t)(got % 41)) intact = 0;
        for (uint32_t i = 0; i < h.payload_len; i++) if (pl[i] != 0x30 + got) intact = 0;
        got++;
    }
    EXPECT_EQ(got, 25);
    EXPECT(ordered);
    EXPECT(intact);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_NEED_MORE);
    flux_frame_reader_free(&rd);

    /* one byte at a time */
    flux_frame_reader_init(&rd);
    got = 0; ordered = 1; intact = 1;
    int bad_midway = 0;
    for (size_t i = 0; i < total; i++) {
        flux_frame_reader_feed(&rd, stream + i, 1);
        frame_status_t st;
        while ((st = flux_frame_reader_next(&rd, &h, &pl)) == FRAME_READY) {
            if (h.id != (uint32_t)(1000 + got)) ordered = 0;
            for (uint32_t k = 0; k < h.payload_len; k++) if (pl[k] != 0x30 + got) intact = 0;
            got++;
        }
        if (st == FRAME_BAD) bad_midway = 1;
    }
    EXPECT_EQ(got, 25);
    EXPECT(ordered && intact);
    EXPECT_EQ(bad_midway, 0);
    flux_frame_reader_free(&rd);

    /* every split point of the first frames */
    int splits_ok = 1;
    for (size_t cut = 1; cut < 200 && cut < total; cut++) {
        flux_frame_reader_init(&rd);
        flux_frame_reader_feed(&rd, stream, cut);
        int n1 = 0;
        while (flux_frame_reader_next(&rd, &h, &pl) == FRAME_READY) n1++;
        flux_frame_reader_feed(&rd, stream + cut, total - cut);
        while (flux_frame_reader_next(&rd, &h, &pl) == FRAME_READY) n1++;
        if (n1 != 25) splits_ok = 0;
        flux_frame_reader_free(&rd);
    }
    EXPECT(splits_ok);

    /* the reserve/commit path used by read(2) */
    flux_frame_reader_init(&rd);
    size_t fed = 0;
    got = 0;
    while (fed < total) {
        uint8_t *dst; size_t space;
        EXPECT(flux_frame_reader_reserve(&rd, &dst, &space) == 0 && space >= 1);
        size_t take = total - fed < 7 ? total - fed : 7;           /* tiny "reads" */
        memcpy(dst, stream + fed, take);
        flux_frame_reader_commit(&rd, take);
        fed += take;
        while (flux_frame_reader_next(&rd, &h, &pl) == FRAME_READY) got++;
    }
    EXPECT_EQ(got, 25);
    flux_frame_reader_free(&rd);

    /* corrupt input is reported, not buffered forever */
    flux_header_t bad;
    memset(&bad, 0, sizeof bad);
    bad.magic = 0xDEADBEEF; bad.version = FLUX_PROTO_VER; bad.type = FLUX_CMD_STATUS;
    flux_frame_reader_init(&rd);
    flux_frame_reader_feed(&rd, &bad, sizeof bad);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    bad.magic = FLUX_MAGIC; bad.version = 9;
    flux_frame_reader_init(&rd);
    flux_frame_reader_feed(&rd, &bad, sizeof bad);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    bad.version = FLUX_PROTO_VER; bad.payload_len = FLUX_MAX_PAYLOAD + 1;
    flux_frame_reader_init(&rd);
    flux_frame_reader_feed(&rd, &bad, sizeof bad);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    /* a maximal frame is accepted, assembled across many small reads */
    size_t maxlen = 16 + FLUX_MAX_PAYLOAD;
    uint8_t *big = malloc(maxlen);
    uint8_t *payload = malloc(FLUX_MAX_PAYLOAD);
    for (size_t i = 0; i < FLUX_MAX_PAYLOAD; i++) payload[i] = (uint8_t)(i * 31u);
    EXPECT_EQ(flux_frame_encode(big, maxlen, FLUX_CMD_START, 0, 77, payload, FLUX_MAX_PAYLOAD), maxlen);
    flux_frame_reader_init(&rd);
    for (size_t off = 0; off < maxlen; off += 3000) {
        size_t take = maxlen - off < 3000 ? maxlen - off : 3000;
        EXPECT_EQ(flux_frame_reader_feed(&rd, big + off, take), 0);
        if (off + take < maxlen && flux_frame_reader_next(&rd, &h, &pl) != FRAME_NEED_MORE) {
            EXPECT(0);                                    /* completed too early */
            break;
        }
    }
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_READY);
    EXPECT_EQ(h.payload_len, FLUX_MAX_PAYLOAD);
    EXPECT(memcmp(pl, payload, FLUX_MAX_PAYLOAD) == 0);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_NEED_MORE);
    /* afterwards an idle reader gives its big buffer back and still works */
    uint8_t *dst; size_t space;
    EXPECT_EQ(flux_frame_reader_reserve(&rd, &dst, &space), 0);
    EXPECT(space >= 4096);
    flux_frame_reader_free(&rd);
    free(big);
    free(payload);

    /* a reader that is freed twice or never used is harmless */
    flux_frame_reader_init(&rd);
    flux_frame_reader_free(&rd);
    flux_frame_reader_free(&rd);
}

static void test_wire_layout(void) {
    SUITE("wire structures");
    EXPECT_EQ(sizeof(flux_header_t), 16);
    EXPECT_EQ(sizeof(flux_rsp_progress_t), 32);
    EXPECT_EQ(sizeof(flux_rsp_status_t), 32);
    EXPECT_EQ(sizeof(flux_rsp_complete_t), 52);
    EXPECT_EQ(sizeof(flux_rsp_error_t), 128);
    EXPECT_EQ(sizeof(flux_cmd_start_t), 56);
    EXPECT_EQ(sizeof(flux_cmd_set_limit_t), 8);
    EXPECT_EQ(FLUX_FLAG_EVENT, 1);
}

/* ── Default socket path ──────────────────────────────────────────────────── */
static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw) {
    (void)sb; (void)type; (void)ftw;
    return remove(path);
}
static void rm_rf(const char *path) { nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS); }

static void make_dir(const char *path, mode_t mode) {
    mkdir(path, mode);
    chmod(path, mode);                                   /* the umask must not get a say */
}

static int mode_of(const char *path) {
    struct stat sb;
    return lstat(path, &sb) == 0 ? (int)(sb.st_mode & 07777) : -1;
}

static const char *err_has(const char *needle) { return strstr(flux_last_error_str(), needle); }

static void test_sock_path(void) {
    SUITE("default socket path (a private directory, never /tmp)");
    int old_level = flux_log_level();
    flux_log_set_level(0);                               /* the "not usable" notice is expected below */

    char root[] = "/tmp/fxsp_XXXXXX";
    if (!mkdtemp(root)) { EXPECT(0); return; }
    char p[1024], q[1024], out[108];

    /* a private XDG_RUNTIME_DIR: the socket goes in <it>/fluxdm */
    snprintf(p, sizeof(p), "%s/run", root);          make_dir(p, 0700);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), 0);
    snprintf(q, sizeof(q), "%s/run/fluxdm/core.sock", root);
    EXPECT_STR(out, q);
    snprintf(q, sizeof(q), "%s/run/fluxdm", root);
    EXPECT_EQ(mode_of(q), 0700);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), 0);   /* and again: nothing to create */

    /* a directory of ours that is too open is closed again */
    snprintf(p, sizeof(p), "%s/run2", root);         make_dir(p, 0700);
    snprintf(q, sizeof(q), "%s/run2/fluxdm", root);  make_dir(q, 0777);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), 0);
    EXPECT_EQ(mode_of(q), 0700);

    /* a symbolic link is refused, and what it points at is left alone */
    snprintf(p, sizeof(p), "%s/run3", root);         make_dir(p, 0700);
    snprintf(q, sizeof(q), "%s/elsewhere", root);    make_dir(q, 0755);
    char link[1024]; snprintf(link, sizeof(link), "%s/run3/fluxdm", root);
    EXPECT_EQ(symlink(q, link), 0);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), -1);
    EXPECT(err_has("not a plain directory") != NULL);
    EXPECT_EQ(mode_of(q), 0755);

    /* a plain file in the way is refused */
    snprintf(p, sizeof(p), "%s/run4", root);         make_dir(p, 0700);
    snprintf(q, sizeof(q), "%s/run4/fluxdm", root);
    FILE *f = fopen(q, "w"); if (f) fclose(f);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), -1);

    /* a directory of somebody else's is refused (needs root to arrange) */
    if (geteuid() == 0) {
        snprintf(p, sizeof(p), "%s/run5", root);         make_dir(p, 0700);
        snprintf(q, sizeof(q), "%s/run5/fluxdm", root);  make_dir(q, 0700);
        EXPECT_EQ(chown(q, 12345, 12345), 0);
        EXPECT_EQ(flux_ipc_resolve_sock_path(p, "/nonexistent", out, sizeof(out)), -1);
        EXPECT(err_has("belongs to another user") != NULL);
    } else {
        printf("  (skipped: the 'belongs to another user' case needs root to set up)\n");
    }

    /* no usable XDG_RUNTIME_DIR: ~/.fluxdm/run */
    char home[512]; snprintf(home, sizeof(home), "%s/home", root);   make_dir(home, 0755);
    snprintf(p, sizeof(p), "%s/open", root);         make_dir(p, 0755);          /* open to others */
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, home, out, sizeof(out)), 0);
    snprintf(q, sizeof(q), "%s/.fluxdm/run/core.sock", home);
    EXPECT_STR(out, q);
    snprintf(q, sizeof(q), "%s/.fluxdm/run", home);
    EXPECT_EQ(mode_of(q), 0700);
    EXPECT_EQ(flux_ipc_resolve_sock_path("relative/run", home, out, sizeof(out)), 0);   /* not absolute */
    EXPECT(strstr(out, "/.fluxdm/run/core.sock") != NULL);
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, home, out, sizeof(out)), 0);             /* not set */
    EXPECT_EQ(flux_ipc_resolve_sock_path("", home, out, sizeof(out)), 0);               /* set but empty */
    snprintf(p, sizeof(p), "%s/missing", root);
    EXPECT_EQ(flux_ipc_resolve_sock_path(p, home, out, sizeof(out)), 0);                /* does not exist */

    /* an existing ~/.fluxdm keeps its mode: the daemon's data lives there */
    char home2[512]; snprintf(home2, sizeof(home2), "%s/home2", root);   make_dir(home2, 0755);
    snprintf(q, sizeof(q), "%s/.fluxdm", home2);     make_dir(q, 0755);
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, home2, out, sizeof(out)), 0);
    EXPECT_EQ(mode_of(q), 0755);
    snprintf(q, sizeof(q), "%s/.fluxdm/run", home2);
    EXPECT_EQ(mode_of(q), 0700);

    /* nothing to go on */
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, NULL, out, sizeof(out)), -1);
    EXPECT(err_has("home directory") != NULL);
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, "relative/home", out, sizeof(out)), -1);

    /* a path that cannot fit sockaddr_un is refused before anything is created */
    char longhome[512]; snprintf(longhome, sizeof(longhome), "%s/%s", root,
        "hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh");
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, longhome, out, sizeof(out)), -1);
    EXPECT(err_has("too long") != NULL);
    snprintf(q, sizeof(q), "%s/.fluxdm", longhome);
    EXPECT_EQ(mode_of(q), -1);
    char tiny[8];
    EXPECT_EQ(flux_ipc_resolve_sock_path(NULL, home, tiny, sizeof(tiny)), -1);          /* the buffer is too small */

    /* flux_ipc_default_sock_path reads the environment */
    char *old_xdg = getenv("XDG_RUNTIME_DIR") ? strdup(getenv("XDG_RUNTIME_DIR")) : NULL;
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    snprintf(p, sizeof(p), "%s/run", root);
    setenv("XDG_RUNTIME_DIR", p, 1);
    setenv("HOME", home, 1);
    EXPECT_EQ(flux_ipc_default_sock_path(out, sizeof(out)), 0);
    snprintf(q, sizeof(q), "%s/run/fluxdm/core.sock", root);
    EXPECT_STR(out, q);
    unsetenv("XDG_RUNTIME_DIR");
    EXPECT_EQ(flux_ipc_default_sock_path(out, sizeof(out)), 0);
    snprintf(q, sizeof(q), "%s/.fluxdm/run/core.sock", home);
    EXPECT_STR(out, q);
    if (old_xdg) setenv("XDG_RUNTIME_DIR", old_xdg, 1); else unsetenv("XDG_RUNTIME_DIR");
    if (old_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    free(old_xdg); free(old_home);

    rm_rf(root);
    flux_log_set_level(old_level);
}

int main(void) {
    printf("\n%s%s FluxDM — Unit Tests (URL, HTTP head, IPC framing, socket path)%s\n", BOLD, CYAN, RESET);
    test_url_parse();
    test_url_resolve();
    test_host_header();
    test_build_get();
    test_parse_head();
    test_read_head();
    test_frame_encode();
    test_frame_reader();
    test_wire_layout();
    test_sock_path();

    printf("\n%s%s─────────────────────────────%s\n", BOLD, CYAN, RESET);
    printf("  %s✔  Passed : %d%s\n", GREEN, g_pass, RESET);
    printf("  %s✘  Failed : %d%s\n", g_fail ? RED : DIM, g_fail, RESET);
    printf("%s%s─────────────────────────────%s\n", BOLD, CYAN, RESET);
    printf("  %s%s%s%s\n\n", BOLD, g_fail ? RED : GREEN, g_fail ? "SOME TESTS FAILED" : "ALL TESTS PASSED", RESET);
    return g_fail ? 1 : 0;
}
