/* FluxDM — tests/test_unit_net.c
 * Unit tests for the protocol layer of the rewritten core: URL parsing and
 * resolution (RFC 3986), the request builder, the response-head parser, the
 * interim-response (1xx) handling, the IPC frame encoder/reader and the
 * limiter's burst depth. Everything here works on plain buffers — no sockets.
 *
 * Build / run:  cd tests && make unit-net && ./test_unit_net
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../core/include/errors.h"
#include "../core/include/bandwidth.h"
#include "../core/include/protocol/http.h"
#include "../core/src/ipc/frame_io.h"

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
    do { if (expr) { printf("  %s✔%s  %s\n", GREEN, RESET, #expr); g_pass++; } \
         else { printf("  %s✘%s  %s  %s(%s:%d)%s\n", RED, RESET, #expr, DIM, __FILE__, __LINE__, RESET); g_fail++; } } while (0)

#define EXPECT_EQ(a, b) \
    do { long long _a = (long long)(a), _b = (long long)(b); \
         if (_a == _b) { printf("  %s✔%s  %s == %s\n", GREEN, RESET, #a, #b); g_pass++; } \
         else { printf("  %s✘%s  %s == %s  (got %lld, expected %lld)  %s(%s:%d)%s\n", RED, RESET, #a, #b, _a, _b, DIM, __FILE__, __LINE__, RESET); g_fail++; } } while (0)

#define EXPECT_STR(a, b) \
    do { const char *_a = (a), *_b = (b); \
         if (strcmp(_a, _b) == 0) { printf("  %s✔%s  \"%s\"\n", GREEN, RESET, _a); g_pass++; } \
         else { printf("  %s✘%s  expected \"%s\", got \"%s\"  %s(%s:%d)%s\n", RED, RESET, _b, _a, DIM, __FILE__, __LINE__, RESET); g_fail++; } } while (0)

/* ── URL parsing ──────────────────────────────────────────────────────────── */
static void test_url_parse(void) {
    SUITE("http_url_parse");
    http_url_t u;

    EXPECT_EQ(http_url_parse("http://example.com/a/b?x=1#frag", &u), 0);
    EXPECT_STR(u.scheme, "http");
    EXPECT_STR(u.host, "example.com");
    EXPECT_EQ(u.port, 80);
    EXPECT_STR(u.path, "/a/b?x=1");
    EXPECT_STR(u.userinfo, "");

    EXPECT_EQ(http_url_parse("HTTPS://Example.COM:8443/X", &u), 0);
    EXPECT_STR(u.scheme, "https");
    EXPECT_STR(u.host, "example.com");                 /* host names are case-insensitive: normalised */
    EXPECT_EQ(u.port, 8443);
    EXPECT_STR(u.path, "/X");                          /* paths are NOT */

    EXPECT_EQ(http_url_parse("https://h", &u), 0);
    EXPECT_EQ(u.port, 443);
    EXPECT_STR(u.path, "/");

    EXPECT_EQ(http_url_parse("http://h?x=1", &u), 0);
    EXPECT_STR(u.path, "/?x=1");
    EXPECT_EQ(http_url_parse("http://h#only-a-fragment", &u), 0);
    EXPECT_STR(u.path, "/");
    EXPECT_EQ(http_url_parse("http://h:/x", &u), 0);
    EXPECT_EQ(u.port, 80);

    EXPECT_EQ(http_url_parse("http://[::1]:8080/x", &u), 0);
    EXPECT_STR(u.host, "::1");
    EXPECT_EQ(u.port, 8080);
    EXPECT_EQ(http_url_parse("http://[2001:DB8::1]/", &u), 0);
    EXPECT_STR(u.host, "2001:db8::1");
    EXPECT_EQ(u.port, 80);

    EXPECT_EQ(http_url_parse("http://user:p%40ss%20w0rd@host.io/f", &u), 0);
    EXPECT_STR(u.userinfo, "user:p@ss w0rd");           /* decoded, ready for Basic auth */
    EXPECT_STR(u.host, "host.io");
    EXPECT_EQ(http_url_parse("http://a@b@host.io/f", &u), 0);
    EXPECT_STR(u.userinfo, "a@b");                      /* the LAST '@' separates the host */
    EXPECT_STR(u.host, "host.io");

    /* Unsafe bytes are encoded the way a browser would; existing escapes stay. */
    EXPECT_EQ(http_url_parse("http://h/a b/c d?q=a b", &u), 0);
    EXPECT_STR(u.path, "/a%20b/c%20d?q=a%20b");
    EXPECT_EQ(http_url_parse("http://h/%2F%e2%82%ac/\xC3\xA9", &u), 0);
    EXPECT_STR(u.path, "/%2F%e2%82%ac/%C3%A9");
    EXPECT_EQ(http_url_parse("  \t http://h/trim\r\n", &u), 0);
    EXPECT_STR(u.path, "/trim");

    /* Rejections */
    EXPECT_EQ(http_url_parse("", &u), -1);
    EXPECT_EQ(http_url_parse("example.com/file", &u), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_HTTP_PARSE);
    EXPECT_EQ(http_url_parse("ftp://h/x", &u), -1);
    EXPECT_EQ(http_url_parse("file:///etc/passwd", &u), -1);
    EXPECT_EQ(http_url_parse("http://", &u), -1);
    EXPECT_EQ(http_url_parse("http:///x", &u), -1);
    EXPECT_EQ(http_url_parse("http://h:0/x", &u), -1);
    EXPECT_EQ(http_url_parse("http://h:65536/x", &u), -1);
    EXPECT_EQ(http_url_parse("http://h:80a/x", &u), -1);
    EXPECT_EQ(http_url_parse("http://[::1/x", &u), -1);
    EXPECT_EQ(http_url_parse("http://[]/x", &u), -1);
    EXPECT_EQ(http_url_parse("http://h\x01st/x", &u), -1);
    EXPECT_EQ(http_url_parse(NULL, &u), -1);

    char big[FLUX_MAX_URL + 64];
    memcpy(big, "http://h/", 9);
    memset(big + 9, 'a', sizeof(big) - 10);
    big[sizeof(big) - 1] = '\0';
    EXPECT_EQ(http_url_parse(big, &u), -1);              /* too long for a request-target */

    char longhost[400];
    memcpy(longhost, "http://", 7);
    memset(longhost + 7, 'h', 300);
    memcpy(longhost + 307, "/x", 3);
    EXPECT_EQ(http_url_parse(longhost, &u), -1);
}

/* ── URL resolution (RFC 3986 §5.4 examples) ──────────────────────────────── */
static void test_url_resolve(void) {
    SUITE("http_url_resolve");
    http_url_t base, r;
    http_url_parse("http://h:8080/a/b/c?q=1", &base);

    struct { const char *loc, *path; } rel[] = {
        { "g",            "/a/b/g"      },
        { "./g",          "/a/b/g"      },
        { "g/",           "/a/b/g/"     },
        { "/g",           "/g"          },
        { "?y",           "/a/b/c?y"    },
        { "g?y",          "/a/b/g?y"    },
        { "#s",           "/a/b/c?q=1"  },
        { "g#s",          "/a/b/g"      },
        { "../g",         "/a/g"        },
        { "../../g",      "/g"          },
        { "../../../g",   "/g"          },
        { "./../g",       "/a/g"        },
        { "g;x=1/../y",   "/a/b/y"      },
        { "..",           "/a/"         },
        { ".",            "/a/b/"       },
    };
    for (size_t i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
        int rc = http_url_resolve(&base, rel[i].loc, &r);
        EXPECT_EQ(rc, 0);
        if (rc == 0) {
            printf("  %s·%s  \"%s\" -> %s\n", DIM, RESET, rel[i].loc, r.path);
            EXPECT_STR(r.path, rel[i].path);
            EXPECT_STR(r.host, "h");
            EXPECT_EQ(r.port, 8080);
        }
    }

    EXPECT_EQ(http_url_resolve(&base, "https://other.example/p?x=1", &r), 0);
    EXPECT_STR(r.scheme, "https");
    EXPECT_STR(r.host, "other.example");
    EXPECT_EQ(r.port, 443);
    EXPECT_STR(r.path, "/p?x=1");

    EXPECT_EQ(http_url_resolve(&base, "//cdn.example:9000/f", &r), 0);   /* scheme-relative */
    EXPECT_STR(r.scheme, "http");
    EXPECT_STR(r.host, "cdn.example");
    EXPECT_EQ(r.port, 9000);

    EXPECT_EQ(http_url_resolve(&base, "", &r), -1);
    EXPECT_EQ(http_url_resolve(&base, "ftp://h/x", &r), -1);

    /* Credentials follow a redirect only on the same host. */
    http_url_t with;
    http_url_parse("http://bob:secret@h:8080/a", &with);
    EXPECT_EQ(http_url_resolve(&with, "/b", &r), 0);
    EXPECT_STR(r.userinfo, "bob:secret");
    EXPECT_EQ(http_url_resolve(&with, "http://h:8080/c", &r), 0);
    EXPECT_STR(r.userinfo, "bob:secret");
    EXPECT_EQ(http_url_resolve(&with, "http://evil.example/c", &r), 0);
    EXPECT_STR(r.userinfo, "");
    EXPECT_EQ(http_url_resolve(&with, "http://h:9999/c", &r), 0);        /* other port = other server */
    EXPECT_STR(r.userinfo, "");
}

/* ── Host header, request builder ─────────────────────────────────────────── */
static void test_request_builder(void) {
    SUITE("Host header and http_build_get");
    http_url_t u;
    char host[300];

    http_url_parse("http://example.com/x", &u);
    EXPECT(http_host_header(&u, host, sizeof(host)) > 0);
    EXPECT_STR(host, "example.com");
    http_url_parse("http://example.com:8080/x", &u);
    http_host_header(&u, host, sizeof(host));
    EXPECT_STR(host, "example.com:8080");
    http_url_parse("https://example.com:443/x", &u);
    http_host_header(&u, host, sizeof(host));
    EXPECT_STR(host, "example.com");
    http_url_parse("https://example.com:80/x", &u);
    http_host_header(&u, host, sizeof(host));
    EXPECT_STR(host, "example.com:80");                 /* 80 is not the default for https */
    http_url_parse("http://[::1]:8080/x", &u);
    http_host_header(&u, host, sizeof(host));
    EXPECT_STR(host, "[::1]:8080");
    http_url_parse("http://[::1]/x", &u);
    http_host_header(&u, host, sizeof(host));
    EXPECT_STR(host, "[::1]");
    EXPECT_EQ(http_host_header(&u, host, 3), -1);

    char buf[2048];
    http_url_parse("http://example.com:8080/dir/f.bin?t=1", &u);
    int n = http_build_get(buf, sizeof(buf), &u, 1, 100, 199);
    EXPECT(n > 0 && (size_t)n == strlen(buf));
    EXPECT(strncmp(buf, "GET /dir/f.bin?t=1 HTTP/1.1\r\n", 29) == 0);
    EXPECT(strstr(buf, "\r\nHost: example.com:8080\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nRange: bytes=100-199\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nAccept-Encoding: identity\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nConnection: close\r\n") != NULL);
    EXPECT(strstr(buf, "\r\nUser-Agent: FluxDM/") != NULL);
    EXPECT(strstr(buf, "Authorization") == NULL);
    EXPECT(n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0);

    n = http_build_get(buf, sizeof(buf), &u, 1, 0, 0);
    EXPECT(strstr(buf, "\r\nRange: bytes=0-0\r\n") != NULL);
    n = http_build_get(buf, sizeof(buf), &u, 1, 5000, UINT64_MAX);
    EXPECT(strstr(buf, "\r\nRange: bytes=5000-\r\n") != NULL);
    n = http_build_get(buf, sizeof(buf), &u, 0, 0, 0);
    EXPECT(n > 0 && strstr(buf, "Range:") == NULL);

    http_url_parse("http://user:p%40ss@example.com/x", &u);
    n = http_build_get(buf, sizeof(buf), &u, 0, 0, 0);
    EXPECT(strstr(buf, "\r\nAuthorization: Basic dXNlcjpwQHNz\r\n") != NULL);   /* base64("user:p@ss") */
    http_url_parse("http://a:b@example.com/x", &u);
    n = http_build_get(buf, sizeof(buf), &u, 0, 0, 0);
    EXPECT(strstr(buf, "Authorization: Basic YTpi\r\n") != NULL);              /* base64("a:b")      */
    http_url_parse("http://ab:c@example.com/x", &u);
    n = http_build_get(buf, sizeof(buf), &u, 0, 0, 0);
    EXPECT(strstr(buf, "Authorization: Basic YWI6Yw==\r\n") != NULL);          /* padding            */

    EXPECT_EQ(http_build_get(buf, 20, &u, 0, 0, 0), -1);                        /* does not fit       */
    (void)n;
}

/* ── Response-head parser ─────────────────────────────────────────────────── */
static int parse(http_response_t *r, const char *s) { return http_parse_head(r, s, strlen(s)); }

static void test_head_parser(void) {
    SUITE("http_parse_head");
    http_response_t r;
    const char *ok =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Length: 100\r\n"
        "Content-Range: bytes 0-99/5000\r\n"
        "Accept-Ranges: bytes\r\n"
        "ETag: \"abc123\"\r\n"
        "Last-Modified: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\nBODY";
    EXPECT_EQ(parse(&r, ok), (int)(strstr(ok, "BODY") - ok));            /* consumed exactly the head */
    EXPECT_EQ(r.status, 206);
    EXPECT_STR(r.reason, "Partial Content");
    EXPECT_EQ(r.version_minor, 1);
    EXPECT(r.has_content_length && r.content_length == 100);
    EXPECT(r.has_content_range && r.content_range_start == 0 && r.content_range_end == 99 &&
           r.content_range_total == 5000);
    EXPECT(r.accept_ranges);
    EXPECT_STR(r.etag, "\"abc123\"");
    EXPECT_STR(r.last_modified, "Wed, 21 Oct 2015 07:28:00 GMT");
    EXPECT(r.keepalive);                                                  /* HTTP/1.1 default */

    /* incomplete -> 0, at every cut point */
    size_t full = (size_t)(strstr(ok, "BODY") - ok);
    int all_zero = 1;
    for (size_t cut = 0; cut < full; cut++) if (http_parse_head(&r, ok, cut) != 0) all_zero = 0;
    EXPECT(all_zero);

    EXPECT(parse(&r, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */1234\r\n\r\n") > 0);
    EXPECT(r.status == 416 && r.content_range_unsat && r.content_range_total == 1234);
    EXPECT(parse(&r, "HTTP/1.1 206 P\r\nContent-Range: bytes 5-9/*\r\n\r\n") > 0);
    EXPECT(r.has_content_range && r.content_range_start == 5 && r.content_range_end == 9 && r.content_range_total == 0);

    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\ncontent-LENGTH:   42  \r\nACCEPT-ranges: BYTES\r\n\r\n") > 0);
    EXPECT(r.has_content_length && r.content_length == 42);               /* names case-insensitive, OWS trimmed */
    EXPECT(r.accept_ranges);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nAccept-Ranges: none\r\n\r\n") > 0);
    EXPECT(!r.accept_ranges);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\n") > 0);
    EXPECT(r.content_length == UINT64_MAX);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n"), -1);   /* overflow */
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: -5\r\n\r\n"), -1);

    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") > 0);
    EXPECT(r.chunked);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\n\r\n") > 0);
    EXPECT(r.chunked);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\n") > 0);
    EXPECT(!r.chunked);

    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n") > 0 && !r.keepalive);
    EXPECT(parse(&r, "HTTP/1.0 200 OK\r\n\r\n") > 0 && !r.keepalive && r.version_minor == 0);
    EXPECT(parse(&r, "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\n\r\n") > 0 && r.keepalive);

    EXPECT(parse(&r, "HTTP/1.1 503 Busy\r\nRetry-After: 7\r\n\r\n") > 0 && r.retry_after_s == 7);
    EXPECT(parse(&r, "HTTP/1.1 503 Busy\r\nRetry-After: Wed, 21 Oct 2015 07:28:00 GMT\r\n\r\n") > 0 && r.retry_after_s == 0);

    EXPECT(parse(&r, "HTTP/1.1 302 Found\r\nLocation: /next?a=1\r\nContent-Length: 0\r\n\r\n") > 0);
    EXPECT_STR(r.location, "/next?a=1");
    EXPECT(parse(&r, "HTTP/1.1 200\r\n\r\n") > 0 && r.status == 200);       /* reason phrase is optional */
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nX-Folded: a\r\n b\r\nContent-Length: 3\r\n\r\n") > 0);
    EXPECT(r.has_content_length && r.content_length == 3);                 /* obs-fold tolerated */
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nWeird Header Without Colon\r\nContent-Length: 3\r\n\r\n") != 0);

    EXPECT_EQ(parse(&r, "garbage\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 20 OK\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 abc\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "HTTP/1.1 1000 Too Big\r\n\r\n"), -1);
    EXPECT_EQ(parse(&r, "SMTP/1.1 200 OK\r\n\r\n"), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_HTTP_PARSE);

    /* An endless head is refused instead of buffered forever. */
    static char huge[HTTP_MAX_HEADER_LEN + 4096];
    memset(huge, 'x', sizeof(huge));
    memcpy(huge, "HTTP/1.1 200 OK\r\nX: ", 20);
    EXPECT_EQ(http_parse_head(&r, huge, sizeof(huge)), -1);

    /* A conflicting pair of Content-Length headers is a broken response. */
    EXPECT_EQ(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"), -1);
    EXPECT(parse(&r, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n") > 0);
}

/* ── flux_http_read_head: consumes the head only, skips 1xx ───────────────── */
static void feed(flux_conn_t *c, const char *s) {
    size_t n = strlen(s);
    memcpy(c->rbuf, s, n);
    c->rbuf_start = 0;
    c->rbuf_end   = n;
}

static void test_read_head(void) {
    SUITE("flux_http_read_head (interim responses, body left in the buffer)");
    flux_conn_t c;
    memset(&c, 0, sizeof(c));
    c.rbuf     = malloc(64 * 1024);
    c.rbuf_cap = 64 * 1024;
    http_response_t r;

    feed(&c, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nDATA");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(c.rbuf_end - c.rbuf_start, 4);
    EXPECT(memcmp(c.rbuf + c.rbuf_start, "DATA", 4) == 0);

    feed(&c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 206 Partial Content\r\nContent-Length: 2\r\n\r\nxy");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 206);                                             /* 100 skipped */
    EXPECT_EQ(c.rbuf_end - c.rbuf_start, 2);

    feed(&c, "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\n"
             "HTTP/1.1 102 Processing\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 200);                                             /* several interim responses */
    EXPECT_EQ(c.rbuf_end - c.rbuf_start, 0);

    feed(&c, "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Le");
    EXPECT_EQ(flux_http_read_head(&c, &r), 0);                            /* the real head is incomplete */

    feed(&c, "HTTP/1.1 101 Switching Protocols\r\n\r\n");
    EXPECT_EQ(flux_http_read_head(&c, &r), 1);
    EXPECT_EQ(r.status, 101);                                             /* 101 is not skipped */

    feed(&c, "this is not http\r\n\r\n");
    EXPECT_EQ(flux_http_read_head(&c, &r), -1);

    feed(&c, "HTTP/1.1 20");
    EXPECT_EQ(flux_http_read_head(&c, &r), 0);
    free(c.rbuf);
}

/* ── Status mapping ───────────────────────────────────────────────────────── */
static void test_status_mapping(void) {
    SUITE("status mapping");
    EXPECT_EQ(http_status_to_flux_error(401), FLUX_ERR_FORBIDDEN);
    EXPECT_EQ(http_status_to_flux_error(403), FLUX_ERR_FORBIDDEN);
    EXPECT_EQ(http_status_to_flux_error(404), FLUX_ERR_NOT_FOUND_);
    EXPECT_EQ(http_status_to_flux_error(410), FLUX_ERR_NOT_FOUND_);
    EXPECT_EQ(http_status_to_flux_error(416), FLUX_ERR_HTTP_RANGE);
    EXPECT_EQ(http_status_to_flux_error(500), FLUX_ERR_SERVER);
    EXPECT_EQ(http_status_to_flux_error(503), FLUX_ERR_SERVER);
    EXPECT_EQ(http_status_to_flux_error(418), FLUX_ERR_HTTP_STATUS);
    EXPECT(http_status_is_retryable(429) && http_status_is_retryable(503) && http_status_is_retryable(408));
    EXPECT(!http_status_is_retryable(404) && !http_status_is_retryable(403) && !http_status_is_retryable(200));
}

/* ── IPC framing ──────────────────────────────────────────────────────────── */
static void test_frame_io(void) {
    SUITE("frame_io: encoder");
    uint8_t buf[256];
    const uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    size_t n = flux_frame_encode(buf, sizeof(buf), FLUX_RSP_PROGRESS, FLUX_FLAG_EVENT, 0x01020304u, payload, 3);
    EXPECT_EQ(n, 16 + 3);
    static const uint8_t want[19] = { 0x58, 0x55, 0x4C, 0x46,       /* "FLUX" magic, little-endian  */
                                      0x01, 0x12,                   /* version, type                */
                                      0x01, 0x00,                   /* flags = EVENT                */
                                      0x04, 0x03, 0x02, 0x01,       /* id                           */
                                      0x03, 0x00, 0x00, 0x00,       /* payload_len                  */
                                      0xAA, 0xBB, 0xCC };
    EXPECT(n == 19 && memcmp(buf, want, 19) == 0);
    EXPECT_EQ(flux_frame_encode(buf, 18, FLUX_RSP_OK, 0, 1, payload, 3), 0);      /* too small */
    EXPECT_EQ(flux_frame_encode(buf, 16, FLUX_RSP_OK, 0, 1, NULL, 0), 16);
    EXPECT_EQ(flux_frame_encode(buf, sizeof(buf), FLUX_RSP_OK, 0, 1, payload, FLUX_MAX_PAYLOAD + 1), 0);

    SUITE("frame_io: reader");
    flux_frame_reader_t rd;
    flux_header_t       h;
    const uint8_t      *pl;

    /* Three frames arriving in one burst. */
    uint8_t burst[200];
    size_t  len = 0;
    len += flux_frame_encode(burst + len, sizeof(burst) - len, FLUX_CMD_STATUS, 0, 7, NULL, 0);
    len += flux_frame_encode(burst + len, sizeof(burst) - len, FLUX_CMD_SET_LIMIT, 0, 0, payload, 3);
    len += flux_frame_encode(burst + len, sizeof(burst) - len, FLUX_CMD_PAUSE, 0, 9, NULL, 0);
    flux_frame_reader_init(&rd);
    EXPECT_EQ(flux_frame_reader_feed(&rd, burst, len), 0);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_READY);
    EXPECT(h.type == FLUX_CMD_STATUS && h.id == 7 && h.payload_len == 0);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_READY);
    EXPECT(h.type == FLUX_CMD_SET_LIMIT && h.payload_len == 3 && memcmp(pl, payload, 3) == 0);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_READY);
    EXPECT(h.type == FLUX_CMD_PAUSE && h.id == 9);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_NEED_MORE);
    flux_frame_reader_free(&rd);

    /* The same bytes one at a time. */
    flux_frame_reader_init(&rd);
    int got = 0, bad = 0;
    for (size_t i = 0; i < len; i++) {
        flux_frame_reader_feed(&rd, burst + i, 1);
        for (;;) {
            frame_status_t st = flux_frame_reader_next(&rd, &h, &pl);
            if (st == FRAME_READY) got++;
            else { if (st == FRAME_BAD) bad++; break; }
        }
    }
    EXPECT(got == 3 && bad == 0);
    flux_frame_reader_free(&rd);

    /* Garbage is detected as soon as a header is complete. */
    flux_frame_reader_init(&rd);
    flux_frame_reader_feed(&rd, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 28);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    flux_frame_reader_init(&rd);                                          /* wrong version */
    flux_frame_encode(buf, sizeof(buf), FLUX_CMD_STATUS, 0, 1, NULL, 0);
    buf[4] = 9;
    flux_frame_reader_feed(&rd, buf, 16);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    flux_frame_reader_init(&rd);                                          /* oversized payload_len */
    flux_frame_encode(buf, sizeof(buf), FLUX_CMD_STATUS, 0, 1, NULL, 0);
    uint32_t huge = FLUX_MAX_PAYLOAD + 1u;
    memcpy(buf + 12, &huge, 4);
    flux_frame_reader_feed(&rd, buf, 16);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_BAD);
    flux_frame_reader_free(&rd);

    flux_frame_reader_init(&rd);                                          /* a partial header waits */
    flux_frame_encode(buf, sizeof(buf), FLUX_CMD_STATUS, 0, 1, NULL, 0);
    flux_frame_reader_feed(&rd, buf, 15);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_NEED_MORE);
    flux_frame_reader_feed(&rd, buf + 15, 1);
    EXPECT_EQ(flux_frame_reader_next(&rd, &h, &pl), FRAME_READY);
    flux_frame_reader_free(&rd);

    /* A maximal frame grows the buffer; an idle reader gives the memory back. */
    uint8_t *big = malloc(16 + FLUX_MAX_PAYLOAD);
    memset(big, 0x5A, 16 + FLUX_MAX_PAYLOAD);
    size_t bn = flux_frame_encode(big, 16 + FLUX_MAX_PAYLOAD, FLUX_CMD_START, 0, 3, big + 16, FLUX_MAX_PAYLOAD);
    EXPECT_EQ(bn, 16 + FLUX_MAX_PAYLOAD);
    flux_frame_reader_init(&rd);
    size_t fed = 0;
    int    ready = 0;
    while (fed < bn) {                                                     /* the way recv() would: reserve, fill, commit */
        uint8_t *dst;
        size_t   space;
        if (flux_frame_reader_reserve(&rd, &dst, &space) < 0) break;
        size_t take = bn - fed < space ? bn - fed : space;
        if (take > 4000) take = 4000;
        memcpy(dst, big + fed, take);
        flux_frame_reader_commit(&rd, take);
        fed += take;
        if (flux_frame_reader_next(&rd, &h, &pl) == FRAME_READY) { ready++; EXPECT(h.payload_len == FLUX_MAX_PAYLOAD && pl[0] == 0x5A && pl[FLUX_MAX_PAYLOAD - 1] == 0x5A); }
    }
    EXPECT_EQ(ready, 1);
    uint8_t *dst;
    size_t   space;
    flux_frame_reader_reserve(&rd, &dst, &space);
    EXPECT(rd.cap <= 64 * 1024 + 4096);                                    /* shrunk back once idle */
    flux_frame_reader_free(&rd);
    free(big);
}

/* ── Limiter depth ────────────────────────────────────────────────────────── */
static void test_bandwidth_burst(void) {
    SUITE("bandwidth: burst depth and debt");
    flux_bucket_t b;
    flux_bw_init(&b, 1024 * 1024);
    flux_bw_set_burst_ms(&b, 250);
    size_t avail = flux_bw_available(&b);
    EXPECT(avail >= 256 * 1024 - 1024 && avail <= 256 * 1024 + 1024);      /* 250 ms of 1 MiB/s */
    EXPECT_EQ(flux_bw_charge(&b, 256 * 1024), 0);                          /* within the burst: no wait */
    uint32_t wait = flux_bw_charge(&b, 256 * 1024);                        /* 256 KiB of debt = 250 ms  */
    EXPECT(wait >= 200 && wait <= 320);
    flux_bw_set_rate(&b, 0);                                               /* unlimited clears the debt */
    EXPECT_EQ(flux_bw_charge(&b, 10 * 1024 * 1024), 0);
    flux_bw_destroy(&b);

    flux_bw_init(&b, 100);                                                 /* absurdly low: the bucket still holds one small read */
    EXPECT_EQ(flux_bw_available(&b), 4096);
    flux_bw_destroy(&b);

    flux_bw_init(&b, 1024 * 1024);                                         /* the default depth is unchanged */
    EXPECT(flux_bw_available(&b) >= 2 * 1024 * 1024 - 4096);
    flux_bw_destroy(&b);
}

int main(void) {
    printf("%s%s\n FluxDM — protocol-layer unit tests%s\n", BOLD, CYAN, RESET);
    test_url_parse();
    test_url_resolve();
    test_request_builder();
    test_head_parser();
    test_read_head();
    test_status_mapping();
    test_frame_io();
    test_bandwidth_burst();

    printf("\n%s%s─────────────────────────────%s\n%s  Results%s\n", BOLD, CYAN, RESET, BOLD, RESET);
    printf("  %s✔  Passed : %d%s\n  %s✘  Failed : %d%s\n", GREEN, g_pass, RESET, g_fail ? RED : DIM, g_fail, RESET);
    printf("%s%s─────────────────────────────%s\n\n", BOLD, CYAN, RESET);
    if (g_fail == 0) printf("  %s%sALL TESTS PASSED%s\n\n", BOLD, GREEN, RESET);
    return g_fail == 0 ? 0 : 1;
}
