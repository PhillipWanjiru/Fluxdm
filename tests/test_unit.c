/* FluxDM — tests/test_unit.c
 * Self-contained unit test runner for all Phase 1 C modules.
 * No external test framework — pure C11.
 *
 * Build:
 *   cd tests && make unit
 * Run:
 *   ./test_unit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
#include <errno.h>

/* Pull in the modules under test directly via source inclusion.
 * This avoids needing a shared library and keeps the test self-contained. */
#include "../core/include/errors.h"
#include "../core/include/bandwidth.h"
#include "../core/include/checksum.h"
#include "../core/include/resume.h"
#include "../core/include/hostgate.h"
#include "../core/include/protocol/http.h"

/* ── Tiny test framework ──────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;
static const char *g_suite = "";

#define RESET  "\033[0m"
#define GREEN  "\033[92m"
#define RED    "\033[91m"
#define YELLOW "\033[93m"
#define CYAN   "\033[96m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"

#define SUITE(name) \
    do { g_suite = name; \
         printf("\n%s%s── %s%s\n", BOLD, CYAN, name, RESET); } while(0)

#define EXPECT(expr) \
    do { \
        if (expr) { \
            printf("  %s✔%s  %s\n", GREEN, RESET, #expr); \
            g_pass++; \
        } else { \
            printf("  %s✘%s  %s  %s(%s:%d)%s\n", \
                   RED, RESET, #expr, DIM, __FILE__, __LINE__, RESET); \
            g_fail++; \
        } \
    } while(0)

#define EXPECT_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a == _b) { \
            printf("  %s✔%s  %s == %s\n", GREEN, RESET, #a, #b); \
            g_pass++; \
        } else { \
            printf("  %s✘%s  %s == %s  (got %lld, expected %lld)  %s(%s:%d)%s\n", \
                   RED, RESET, #a, #b, _a, _b, DIM, __FILE__, __LINE__, RESET); \
            g_fail++; \
        } \
    } while(0)

#define EXPECT_STR(a, b) \
    do { \
        if (strcmp((a),(b)) == 0) { \
            printf("  %s✔%s  \"%s\"\n", GREEN, RESET, (a)); \
            g_pass++; \
        } else { \
            printf("  %s✘%s  expected \"%s\", got \"%s\"  %s(%s:%d)%s\n", \
                   RED, RESET, (b), (a), DIM, __FILE__, __LINE__, RESET); \
            g_fail++; \
        } \
    } while(0)

/* ── Suite 1: errors ──────────────────────────────────────────────────────── */
static void test_errors(void) {
    SUITE("errors.c");

    EXPECT_EQ(flux_last_error(), FLUX_OK);

    flux_set_error(FLUX_ERR_NOMEM, "out of memory: %d bytes", 1024);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_NOMEM);
    EXPECT(strstr(flux_last_error_str(), "1024") != NULL);

    flux_set_error(FLUX_ERR_DNS, NULL);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_DNS);
    EXPECT(strlen(flux_last_error_str()) > 0);

    flux_clear_error();
    EXPECT_EQ(flux_last_error(), FLUX_OK);

    /* strerror table */
    EXPECT_STR(flux_strerror(FLUX_OK),            "OK");
    EXPECT_STR(flux_strerror(FLUX_ERR_NOMEM),     "Out of memory");
    EXPECT_STR(flux_strerror(FLUX_ERR_DNS),       "DNS resolution failed");
    EXPECT_STR(flux_strerror(FLUX_ERR_DISK_WRITE),"Disk write failed");
    EXPECT_STR(flux_strerror(FLUX_ERR_UNKNOWN),   "Unknown error");

    /* syserror */
    errno = ENOENT;
    flux_set_syserror("test_open");
    EXPECT_EQ(flux_last_error(), FLUX_ERR_SYS);
    EXPECT(strstr(flux_last_error_str(), "test_open") != NULL);
}

/* ── Suite 2: errors — thread isolation ───────────────────────────────────── */
typedef struct { flux_error_t code; int done; } thread_err_ctx_t;

static void *thread_err_fn(void *arg) {
    thread_err_ctx_t *ctx = arg;
    flux_set_error(ctx->code, "thread error %d", (int)ctx->code);
    usleep(10000);   /* let the main thread check its own state */
    ctx->done = 1;
    return NULL;
}

static void test_errors_thread_isolation(void) {
    SUITE("errors.c — thread isolation");

    flux_clear_error();
    EXPECT_EQ(flux_last_error(), FLUX_OK);

    thread_err_ctx_t ctx = { .code = FLUX_ERR_TLS_HNDSHK, .done = 0 };
    pthread_t t;
    pthread_create(&t, NULL, thread_err_fn, &ctx);
    usleep(5000);

    /* Main thread's error must be unaffected by the child thread */
    EXPECT_EQ(flux_last_error(), FLUX_OK);

    pthread_join(t, NULL);
    EXPECT_EQ(ctx.done, 1);
}

/* ── Suite 3: bandwidth ───────────────────────────────────────────────────── */
static void test_bandwidth(void) {
    SUITE("bandwidth.c — token bucket");

    flux_bucket_t b;

    /* Unlimited: always succeeds */
    flux_bw_init(&b, 0);
    EXPECT_EQ(flux_bw_available(&b), (size_t)SIZE_MAX);
    uint32_t wait = 0;
    EXPECT_EQ(flux_bw_consume(&b, 1024 * 1024, &wait), 0);
    EXPECT_EQ(flux_bw_consume(&b, 1024 * 1024, &wait), 0);
    flux_bw_destroy(&b);

    /* Limited: 1 MB/s — bucket starts full at 2 MB burst */
    flux_bw_init(&b, 1024 * 1024);
    EXPECT(flux_bw_available(&b) >= (size_t)(1024 * 1024));

    /* Consume 1 MB — should succeed (within burst) */
    EXPECT_EQ(flux_bw_consume(&b, 1024 * 1024, &wait), 0);

    /* Consume 1 MB more — should succeed (burst is 2×rate) */
    EXPECT_EQ(flux_bw_consume(&b, 1024 * 1024, &wait), 0);

    /* Consume yet more — should throttle */
    int rc = flux_bw_consume(&b, 1024 * 1024, &wait);
    EXPECT_EQ(rc, -1);
    EXPECT(wait > 0);
    printf("  %s→%s  throttled: retry in %u ms\n", DIM, RESET, wait);

    /* Wait for refill and try again */
    usleep((useconds_t)wait * 1100);
    flux_bw_refill(&b);
    EXPECT_EQ(flux_bw_consume(&b, 512 * 1024, &wait), 0);

    /* Change rate at runtime */
    flux_bw_set_rate(&b, 512 * 1024);
    EXPECT_EQ(b.rate_bps, (uint64_t)(512 * 1024));

    flux_bw_destroy(&b);
}

/* ── Suite 4: checksum ────────────────────────────────────────────────────── */
static void test_checksum(void) {
    SUITE("checksum.c — SHA-256 / MD5");

    /* Known SHA-256 of "FluxDM" */
    const char *input = "FluxDM";
    /* echo -n "FluxDM" | sha256sum */
    const char *expected_sha256 =
        "dc35fcee242061c7b444071f54c34c5ab4baf04c34ed2f632f1ab285b765e3dd";
    /* echo -n "FluxDM" | md5sum */
    const char *expected_md5 = "340b39480de86141b76da70aa593dcf4";

    /* SHA-256 streaming */
    flux_cksum_ctx_t *ctx = flux_cksum_new(FLUX_CKSUM_SHA256);
    EXPECT(ctx != NULL);
    if (ctx) {
        EXPECT_EQ(flux_cksum_update(ctx, input, strlen(input)), 0);
        uint8_t digest[32]; size_t dlen = 0;
        EXPECT_EQ(flux_cksum_final(ctx, digest, &dlen), 0);
        EXPECT_EQ(dlen, (size_t)32);
        char hex[65];
        flux_cksum_hex(digest, dlen, hex);
        EXPECT_STR(hex, expected_sha256);

        /* Reuse after final */
        EXPECT_EQ(flux_cksum_update(ctx, input, strlen(input)), 0);
        EXPECT_EQ(flux_cksum_final(ctx, digest, &dlen), 0);
        flux_cksum_hex(digest, dlen, hex);
        EXPECT_STR(hex, expected_sha256);

        flux_cksum_free(ctx);
    }

    /* MD5 */
    ctx = flux_cksum_new(FLUX_CKSUM_MD5);
    EXPECT(ctx != NULL);
    if (ctx) {
        EXPECT_EQ(flux_cksum_update(ctx, input, strlen(input)), 0);
        uint8_t digest[16]; size_t dlen = 0;
        EXPECT_EQ(flux_cksum_final(ctx, digest, &dlen), 0);
        EXPECT_EQ(dlen, (size_t)16);
        char hex[33];
        flux_cksum_hex(digest, dlen, hex);
        EXPECT_STR(hex, expected_md5);
        flux_cksum_free(ctx);
    }

    /* File checksum */
    const char *tmpfile = "/tmp/fluxdm_cksum_test.bin";
    FILE *f = fopen(tmpfile, "wb");
    if (f) {
        fwrite(input, 1, strlen(input), f);
        fclose(f);
        uint8_t digest[32]; size_t dlen = 0;
        EXPECT_EQ(flux_cksum_file(tmpfile, FLUX_CKSUM_SHA256, digest, &dlen), 0);
        char hex[65]; flux_cksum_hex(digest, dlen, hex);
        EXPECT_STR(hex, expected_sha256);
        unlink(tmpfile);
    }

    /* Verify helper */
    ctx = flux_cksum_new(FLUX_CKSUM_SHA256);
    if (ctx) {
        flux_cksum_update(ctx, input, strlen(input));
        uint8_t expected_bytes[32];
        for (int i = 0; i < 32; i++) {
            unsigned int byte;
            sscanf(expected_sha256 + i * 2, "%02x", &byte);
            expected_bytes[i] = (uint8_t)byte;
        }
        EXPECT_EQ(flux_cksum_verify(ctx, expected_bytes, 32), 1);

        /* Wrong expected — should fail */
        flux_cksum_update(ctx, input, strlen(input));
        expected_bytes[0] ^= 0xFF;
        EXPECT_EQ(flux_cksum_verify(ctx, expected_bytes, 32), 0);
        flux_cksum_free(ctx);
    }
}

/* ── Suite 5: resume ──────────────────────────────────────────────────────── */
static void test_resume(void) {
    SUITE("resume.c — save/load round-trip");

    const char *part_path = "/tmp/fluxdm_test.fluxpart";
    unlink(part_path);

    flux_resume_t r_out = {0};
    r_out.hdr.magic        = FLUX_RESUME_MAGIC;
    r_out.hdr.version      = FLUX_RESUME_VER;
    r_out.hdr.download_id  = 42;
    r_out.hdr.num_segments = 4;
    r_out.hdr.total_size   = 1024 * 1024 * 100;  /* 100 MB */
    r_out.hdr.checksum_type = 2;  /* SHA256 */
    strncpy(r_out.url,  "https://example.com/bigfile.iso", FLUX_RESUME_MAX_URL - 1);
    strncpy(r_out.path, "/home/user/Downloads/bigfile.iso", FLUX_RESUME_MAX_URL - 1);

    for (uint32_t i = 0; i < 4; i++) {
        r_out.segs[i].range_start = i * (1024 * 1024 * 25);
        r_out.segs[i].range_end   = r_out.segs[i].range_start + (1024 * 1024 * 25) - 1;
        r_out.segs[i].written     = i * 1024 * 100;   /* varying progress */
        r_out.segs[i].status      = (i == 0) ? 1 : 0; /* segment 0 complete */
    }

    EXPECT_EQ(flux_resume_save(&r_out, part_path), 0);
    EXPECT_EQ(access(part_path, F_OK), 0);

    /* Load back */
    flux_resume_t r_in = {0};
    EXPECT_EQ(flux_resume_load(&r_in, part_path), 0);
    EXPECT_EQ((int)r_in.hdr.magic,       (int)FLUX_RESUME_MAGIC);
    EXPECT_EQ((int)r_in.hdr.version,     (int)FLUX_RESUME_VER);
    EXPECT_EQ((int)r_in.hdr.download_id, 42);
    EXPECT_EQ((int)r_in.hdr.num_segments, 4);
    EXPECT_EQ(r_in.hdr.total_size, (uint64_t)(1024 * 1024 * 100));
    EXPECT_STR(r_in.url,  r_out.url);
    EXPECT_STR(r_in.path, r_out.path);
    EXPECT_EQ((int)r_in.segs[0].status,  1);  /* complete */
    EXPECT_EQ((int)r_in.segs[1].status,  0);  /* in progress */
    EXPECT_EQ(r_in.segs[2].written, r_out.segs[2].written);
    EXPECT_EQ(r_in.segs[3].range_end, r_out.segs[3].range_end);

    /* Corrupt file detection */
    FILE *f = fopen(part_path, "r+b");
    if (f) { fseek(f, 0, SEEK_SET); fwrite("JUNK", 4, 1, f); fclose(f); }
    EXPECT_EQ(flux_resume_load(&r_in, part_path), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_DISK_READ);

    /* Missing file */
    unlink(part_path);
    EXPECT_EQ(flux_resume_load(&r_in, part_path), -1);

    /* Delete helper */
    flux_resume_save(&r_out, part_path);
    flux_resume_delete(part_path);
    EXPECT_EQ(access(part_path, F_OK), -1);

    /* Path helper */
    char ppath[512];
    flux_resume_path("/downloads/video.mp4", ppath, sizeof(ppath));
    EXPECT_STR(ppath, "/downloads/video.mp4.fluxpart");
}

/* ── Suite 6: HTTP URL parser ─────────────────────────────────────────────── */
static void test_http_url_parse(void) {
    SUITE("http.c — URL parser");

    http_url_t u;

    /* Plain HTTP */
    EXPECT_EQ(http_url_parse("http://example.com/path/to/file.zip", &u), 0);
    EXPECT_STR(u.scheme, "http");
    EXPECT_STR(u.host,   "example.com");
    EXPECT_EQ(u.port,    80);
    EXPECT_STR(u.path,   "/path/to/file.zip");

    /* HTTPS with port */
    EXPECT_EQ(http_url_parse("https://files.server.net:8443/dl/a.tar.gz", &u), 0);
    EXPECT_STR(u.scheme, "https");
    EXPECT_STR(u.host,   "files.server.net");
    EXPECT_EQ(u.port,    8443);
    EXPECT_STR(u.path,   "/dl/a.tar.gz");

    /* No path (root) */
    EXPECT_EQ(http_url_parse("https://example.com", &u), 0);
    EXPECT_STR(u.path, "/");

    /* Query string preserved in path */
    EXPECT_EQ(http_url_parse("http://cdn.host.io/file?token=abc&exp=123", &u), 0);
    EXPECT(strstr(u.path, "?token=abc") != NULL);

    /* Default HTTPS port */
    EXPECT_EQ(http_url_parse("https://secure.host.com/file", &u), 0);
    EXPECT_EQ(u.port, 443);

    /* Invalid — no scheme */
    EXPECT_EQ(http_url_parse("example.com/file", &u), -1);
    EXPECT_EQ(flux_last_error(), FLUX_ERR_HTTP_PARSE);
}

/* ── Suite 7: HTTP header parser ──────────────────────────────────────────── */
static void test_http_headers(void) {
    SUITE("http.c — header parser");

    /* We test parse_headers_buf indirectly via flux_http_parse_head_response.
     * Build a fake connection with a buffered HTTP response. */

    const char *resp_200 =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 52428800\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: keep-alive\r\n"
        "ETag: \"abc123\"\r\n"
        "\r\n";

    /* Manually fill a connection's ring buffer */
    flux_conn_t conn = {0};
    conn.rbuf     = malloc(512 * 1024);
    conn.rbuf_cap = 512 * 1024;
    size_t rlen   = strlen(resp_200);
    memcpy(conn.rbuf, resp_200, rlen);
    conn.rbuf_end = rlen;

    http_response_t r = {0};
    EXPECT_EQ(flux_http_parse_head_response(&conn, &r), 1);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.content_length, (uint64_t)(52428800));
    EXPECT_EQ(r.accept_ranges,  1);
    EXPECT_EQ(r.keepalive,      1);
    EXPECT_STR(r.content_type, "application/octet-stream");
    EXPECT_STR(r.etag, "\"abc123\"");

    /* 206 Partial Content */
    const char *resp_206 =
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-26214399/52428800\r\n"
        "Content-Length: 26214400\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    memcpy(conn.rbuf, resp_206, strlen(resp_206));
    conn.rbuf_start = 0;
    conn.rbuf_end   = strlen(resp_206);
    memset(&r, 0, sizeof(r));

    EXPECT_EQ(flux_http_parse_head_response(&conn, &r), 1);
    EXPECT_EQ(r.status, 206);
    EXPECT_EQ(r.content_range_start, (uint64_t)0);
    EXPECT_EQ(r.content_range_end,   (uint64_t)26214399);
    EXPECT_EQ(r.content_range_total, (uint64_t)52428800);

    /* 404 */
    const char *resp_404 =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    memcpy(conn.rbuf, resp_404, strlen(resp_404));
    conn.rbuf_start = 0;
    conn.rbuf_end   = strlen(resp_404);
    memset(&r, 0, sizeof(r));
    EXPECT_EQ(flux_http_parse_head_response(&conn, &r), 1);
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(http_status_to_flux_error(404), FLUX_ERR_NOT_FOUND_);

    /* Incomplete headers — should return 0 */
    const char *partial = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n";
    memcpy(conn.rbuf, partial, strlen(partial));
    conn.rbuf_start = 0;
    conn.rbuf_end   = strlen(partial);
    memset(&r, 0, sizeof(r));
    EXPECT_EQ(flux_http_parse_head_response(&conn, &r), 0);

    free(conn.rbuf);
}

/* ── Summary ──────────────────────────────────────────────────────────────── */
/* ── Suite 8: hostgate — per-host connection governor ─────────────────────── */
#define T0 1000000ull                       /* an arbitrary "now" (ms) */

#define GATE_STATS(host, port, lim, act) \
    do { (lim) = 0xFFFFu; (act) = 0xFFFFu; flux_gate_stats((host), (port), &(lim), &(act)); } while (0)

static void test_hostgate_slots(void) {
    SUITE("hostgate.c — slots");
    flux_gate_reset();

    flux_hostgate_t *g = NULL;
    uint32_t wait = 99, limit, active;

    EXPECT_EQ(flux_gate_stats("a.example", 80, &limit, &active), -1);          /* unknown until first use */
    EXPECT_EQ(flux_gate_acquire("a.example", 80, T0, &g, &wait), 1);
    EXPECT(g != NULL);
    EXPECT_EQ(wait, 0);
    GATE_STATS("a.example", 80, limit, active);
    EXPECT_EQ(limit, HOSTGATE_OPEN);
    EXPECT_EQ(active, 1);

    flux_gate_release(g);
    GATE_STATS("a.example", 80, limit, active);
    EXPECT_EQ(active, 0);
    flux_gate_release(g);                                                       /* over-release is harmless */
    flux_gate_release(NULL);                                                    /* so is NULL */
    GATE_STATS("a.example", 80, limit, active);
    EXPECT_EQ(active, 0);

    /* host names are case-insensitive; the port is part of the identity */
    flux_gate_reset();
    flux_hostgate_t *g1 = NULL, *g2 = NULL, *g3 = NULL, *g4 = NULL;
    EXPECT_EQ(flux_gate_acquire("Example.COM", 443, T0, &g1, &wait), 1);
    EXPECT_EQ(flux_gate_acquire("example.com", 443, T0, &g2, &wait), 1);
    EXPECT(g1 == g2);
    EXPECT_EQ(flux_gate_acquire("example.com", 80, T0, &g3, &wait), 1);
    EXPECT(g3 != g1);
    EXPECT_EQ(flux_gate_acquire("other.example", 443, T0, &g4, &wait), 1);
    EXPECT(g4 != g1 && g4 != g3);
    GATE_STATS("example.com", 443, limit, active);
    EXPECT_EQ(active, 2);

    /* a host that has not complained may use every slot of the open allowance */
    flux_gate_reset();
    unsigned granted = 0;
    for (unsigned i = 0; i < HOSTGATE_OPEN; i++)
        granted += (unsigned)flux_gate_acquire("busy.example", 80, T0, &g, &wait);
    EXPECT_EQ(granted, HOSTGATE_OPEN);
    EXPECT_EQ(flux_gate_acquire("busy.example", 80, T0, &g, &wait), 0);
    EXPECT(g == NULL);
    EXPECT_EQ(wait, HOSTGATE_POLL_MS);                                          /* ask again soon */

    /* hold() counts a connection that already exists, even over the allowance */
    flux_gate_reset();
    flux_hostgate_t *slot[4];
    for (int i = 0; i < 4; i++) flux_gate_acquire("h.example", 80, T0, &slot[i], &wait);
    flux_gate_throttled(slot[3], T0, 0);                                        /* allowance becomes 3 */
    flux_gate_release(slot[3]);
    flux_hostgate_t *held = NULL;
    flux_gate_hold("h.example", 80, T0 + 1, &held);
    EXPECT(held == slot[0]);
    GATE_STATS("h.example", 80, limit, active);
    EXPECT_EQ(limit, 3);
    EXPECT_EQ(active, 4);                                                       /* over the allowance, but counted */
    flux_gate_release(held);
    GATE_STATS("h.example", 80, limit, active);
    EXPECT_EQ(active, 3);
}

static void test_hostgate_refusals(void) {
    SUITE("hostgate.c — a refusal lowers the allowance");
    flux_gate_reset();

    /* Three downloads with eight segments each: 24 connections at once. The
     * server takes 8 and refuses 16; every refused segment reports it and gives
     * its slot back. The allowance ends exactly where the server's tolerance is. */
    flux_hostgate_t *s[24];
    uint32_t wait, limit, active;
    unsigned granted = 0;
    for (int i = 0; i < 24; i++) granted += (unsigned)flux_gate_acquire("h.example", 443, T0, &s[i], &wait);
    EXPECT_EQ(granted, 24);

    uint32_t first = flux_gate_throttled(s[8], T0 + 50, 0);
    flux_gate_release(s[8]);
    EXPECT_EQ(first, 23);                                                       /* 23 others coexisted with it */
    uint32_t last = 0;
    for (int i = 9; i < 24; i++) {
        uint32_t lowered = flux_gate_throttled(s[i], T0 + 50, 0);
        if (lowered) last = lowered;
        flux_gate_release(s[i]);
    }
    GATE_STATS("h.example", 443, limit, active);
    EXPECT_EQ(last, 8);
    EXPECT_EQ(limit, 8);
    EXPECT_EQ(active, 8);

    /* the refused segments come back: no slot until one of the eight finishes */
    flux_hostgate_t *g = NULL;
    EXPECT_EQ(flux_gate_acquire("h.example", 443, T0 + 1000, &g, &wait), 0);
    EXPECT_EQ(wait, HOSTGATE_POLL_MS);
    flux_gate_release(s[0]);
    EXPECT_EQ(flux_gate_acquire("h.example", 443, T0 + 1100, &g, &wait), 1);
    EXPECT(g != NULL);

    /* refused again, now with 7 others open: the allowance follows it down... */
    EXPECT_EQ(flux_gate_throttled(g, T0 + 1100, 0), 7);
    /* ...and a refusal that tells us nothing new reports nothing */
    EXPECT_EQ(flux_gate_throttled(s[1], T0 + 1100, 0), 0);

    /* the floor is one connection: a host is never starved completely */
    flux_gate_reset();
    flux_hostgate_t *only = NULL;
    flux_gate_acquire("slow.example", 80, T0, &only, &wait);
    EXPECT_EQ(flux_gate_throttled(only, T0, 0), 1);
    GATE_STATS("slow.example", 80, limit, active);
    EXPECT_EQ(limit, HOSTGATE_MIN);
    EXPECT_EQ(flux_gate_throttled(only, T0 + 10, 0), 0);                        /* already at the floor */
    GATE_STATS("slow.example", 80, limit, active);
    EXPECT_EQ(limit, HOSTGATE_MIN);

    /* a request without a slot (the probe) is judged against the connections that are open */
    flux_gate_reset();
    flux_hostgate_t *t[5];
    for (int i = 0; i < 5; i++) flux_gate_acquire("p.example", 80, T0, &t[i], &wait);
    EXPECT_EQ(flux_gate_note_refusal("p.example", 80, T0 + 5, 0), 5);
    GATE_STATS("p.example", 80, limit, active);
    EXPECT_EQ(limit, 5);
    EXPECT_EQ(flux_gate_throttled(NULL, T0, 0), 0);                             /* an untracked connection */
}

static void test_hostgate_pause_and_pacing(void) {
    SUITE("hostgate.c — Retry-After and pacing");
    flux_gate_reset();

    flux_hostgate_t *g = NULL;
    uint32_t wait = 0;

    /* Retry-After pauses the whole host, for everybody */
    flux_gate_acquire("r.example", 80, T0, &g, &wait);
    flux_gate_throttled(g, T0, 5000);
    flux_gate_release(g);
    EXPECT_EQ(flux_gate_pause_ms("r.example", 80, T0 + 1000), 4000);
    EXPECT_EQ(flux_gate_acquire("r.example", 80, T0 + 1000, &g, &wait), 0);
    EXPECT(g == NULL);
    EXPECT_EQ(wait, 4000);
    EXPECT_EQ(flux_gate_pause_ms("r.example", 80, T0 + 5000), 0);
    EXPECT_EQ(flux_gate_pause_ms("unknown.example", 80, T0), 0);

    /* a shorter Retry-After never shortens a pause that is already running */
    flux_hostgate_t *g2 = NULL;
    flux_gate_hold("r.example", 80, T0 + 100, &g2);
    flux_gate_throttled(g2, T0 + 100, 1000);
    flux_gate_release(g2);
    EXPECT_EQ(flux_gate_pause_ms("r.example", 80, T0 + 1000), 4000);

    /* while the host is unhappy, new connections are spaced out */
    flux_gate_reset();
    flux_hostgate_t *sl[4];
    for (int i = 0; i < 4; i++) flux_gate_acquire("g.example", 80, T0, &sl[i], &wait);
    flux_gate_throttled(sl[3], T0, 0);                                          /* allowance 3 */
    flux_gate_release(sl[3]);
    flux_gate_release(sl[2]);                                                   /* 2 open, room for one */
    EXPECT_EQ(flux_gate_acquire("g.example", 80, T0 + 10, &g, &wait), 1);
    flux_gate_release(g);
    EXPECT_EQ(flux_gate_acquire("g.example", 80, T0 + 20, &g, &wait), 0);
    EXPECT_EQ(wait, HOSTGATE_GAP_MS - 10);
    EXPECT_EQ(flux_gate_acquire("g.example", 80, T0 + 10 + HOSTGATE_GAP_MS, &g, &wait), 1);
}

static void test_hostgate_recovery(void) {
    SUITE("hostgate.c — the allowance recovers");
    flux_gate_reset();

    flux_hostgate_t *sl[4], *g = NULL;
    uint32_t wait, limit, active;
    for (int i = 0; i < 4; i++) flux_gate_acquire("c.example", 80, T0, &sl[i], &wait);
    flux_gate_throttled(sl[3], T0, 0);                                          /* allowance 3, three open */
    flux_gate_release(sl[3]);

    /* nothing grows while the host is unhappy... */
    EXPECT_EQ(flux_gate_acquire("c.example", 80, T0 + HOSTGATE_CALM_MS - 1, &g, &wait), 0);
    GATE_STATS("c.example", 80, limit, active);
    EXPECT_EQ(limit, 3);

    /* ...after a calm stretch, somebody waiting earns one more connection, then one per interval */
    uint64_t t = T0 + HOSTGATE_CALM_MS + HOSTGATE_RAISE_MS;
    EXPECT_EQ(flux_gate_acquire("c.example", 80, t, &g, &wait), 1);
    GATE_STATS("c.example", 80, limit, active);
    EXPECT_EQ(limit, 4);
    EXPECT_EQ(active, 4);
    EXPECT_EQ(flux_gate_acquire("c.example", 80, t + 1, &g, &wait), 0);         /* not again so soon */
    EXPECT_EQ(flux_gate_acquire("c.example", 80, t + HOSTGATE_RAISE_MS, &g, &wait), 1);
    GATE_STATS("c.example", 80, limit, active);
    EXPECT_EQ(limit, 5);

    /* growth stops at the open allowance */
    flux_gate_reset();
    flux_hostgate_t *one = NULL;
    flux_gate_acquire("d.example", 80, T0, &one, &wait);
    flux_gate_throttled(one, T0, 0);                                            /* allowance 1 */
    uint64_t now = T0;
    for (int i = 0; i < 300; i++) {
        now += HOSTGATE_CALM_MS + HOSTGATE_RAISE_MS;
        flux_gate_acquire("d.example", 80, now, &g, &wait);
    }
    GATE_STATS("d.example", 80, limit, active);
    EXPECT_EQ(limit, HOSTGATE_OPEN);

    /* an idle host's complaints are forgotten in the end */
    flux_gate_reset();
    for (int i = 0; i < 4; i++) flux_gate_acquire("f.example", 80, T0, &sl[i], &wait);
    flux_gate_throttled(sl[3], T0, 60000);
    for (int i = 0; i < 4; i++) flux_gate_release(sl[i]);
    GATE_STATS("f.example", 80, limit, active);
    EXPECT_EQ(limit, 3);
    EXPECT_EQ(flux_gate_acquire("f.example", 80, T0 + HOSTGATE_FORGET_MS - 1, &g, &wait), 1);
    GATE_STATS("f.example", 80, limit, active);
    EXPECT_EQ(limit, 3);
    flux_gate_release(g);
    EXPECT_EQ(flux_gate_acquire("f.example", 80, T0 + HOSTGATE_FORGET_MS, &g, &wait), 1);
    GATE_STATS("f.example", 80, limit, active);
    EXPECT_EQ(limit, HOSTGATE_OPEN);
    EXPECT_EQ(flux_gate_pause_ms("f.example", 80, T0 + HOSTGATE_FORGET_MS), 0);
}

static void test_hostgate_table(void) {
    SUITE("hostgate.c — a full table");
    flux_gate_reset();

    flux_hostgate_t *held[64], *g = NULL;
    uint32_t wait, limit, active;
    char host[32];
    unsigned granted = 0;
    for (int i = 0; i < 64; i++) {
        snprintf(host, sizeof(host), "h%d.example", i);
        granted += (unsigned)flux_gate_acquire(host, 80, T0 + (uint64_t)i, &held[i], &wait);
    }
    EXPECT_EQ(granted, 64);

    /* every row busy: the 65th host is simply not tracked (no gating, no failure) */
    EXPECT_EQ(flux_gate_acquire("overflow.example", 80, T0 + 100, &g, &wait), 1);
    EXPECT(g == NULL);
    EXPECT_EQ(flux_gate_stats("overflow.example", 80, &limit, &active), -1);
    flux_gate_release(g);

    /* an idle row is recycled — the stalest one first */
    flux_gate_release(held[9]);
    flux_gate_release(held[5]);
    EXPECT_EQ(flux_gate_acquire("new.example", 80, T0 + 200, &g, &wait), 1);
    EXPECT(g != NULL);
    EXPECT_EQ(flux_gate_stats("h5.example", 80, &limit, &active), -1);
    EXPECT_EQ(flux_gate_stats("h9.example", 80, &limit, &active), 0);
    EXPECT_EQ(flux_gate_stats("h0.example", 80, &limit, &active), 0);           /* busy rows are never evicted */
}

static void test_hostgate_policy(void) {
    SUITE("hostgate.c — back-off and patience");

    /* 1 s, 2 s, 4 s, 8 s, 16 s, then 30 s — each within its +-12% jitter */
    unsigned base[] = { 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    int in_range = 1;
    for (unsigned n = 0; n < 8; n++)
        for (uint32_t salt = 0; salt < 64; salt++) {
            uint32_t ms = flux_gate_backoff_ms(n, salt);
            if (ms < base[n] * 88u / 100u || ms > base[n] * 112u / 100u) in_range = 0;
        }
    EXPECT(in_range);
    EXPECT(flux_gate_backoff_ms(1000000u, 3) <= 33600u);

    /* siblings do not all come back at the same instant */
    uint32_t distinct = 0, seen[64];
    for (uint32_t salt = 0; salt < 64; salt++) {
        uint32_t ms = flux_gate_backoff_ms(2, salt);
        int dup = 0;
        for (uint32_t i = 0; i < distinct; i++) if (seen[i] == ms) dup = 1;
        if (!dup) seen[distinct++] = ms;
    }
    EXPECT(distinct >= 8);

    EXPECT_EQ(flux_gate_patience_ms(), HOSTGATE_PATIENCE_MS);
    flux_gate_set_patience_ms(2500);
    EXPECT_EQ(flux_gate_patience_ms(), 2500);
    flux_gate_set_patience_ms(HOSTGATE_PATIENCE_MS);
}

#define GATE_THREADS 8
#define GATE_ROUNDS  20000

typedef struct { int id; int with_refusals; long granted; } gate_job_t;

static void *gate_thread(void *arg) {
    gate_job_t *job = arg;
    for (int i = 0; i < GATE_ROUNDS; i++) {
        flux_hostgate_t *g = NULL;
        uint32_t wait;
        if (flux_gate_acquire("race.example", 80, T0 + (uint64_t)i, &g, &wait)) {
            job->granted++;
            if (job->with_refusals && (i % 37) == job->id) flux_gate_throttled(g, T0 + (uint64_t)i, 0);
            flux_gate_release(g);
        }
    }
    return NULL;
}

static void test_hostgate_threads(void) {
    SUITE("hostgate.c — threads");

    for (int mode = 0; mode < 2; mode++) {
        flux_gate_reset();
        pthread_t th[GATE_THREADS];
        gate_job_t job[GATE_THREADS];
        for (int i = 0; i < GATE_THREADS; i++) {
            job[i] = (gate_job_t){ .id = i, .with_refusals = mode };
            pthread_create(&th[i], NULL, gate_thread, &job[i]);
        }
        long granted = 0;
        for (int i = 0; i < GATE_THREADS; i++) { pthread_join(th[i], NULL); granted += job[i].granted; }

        uint32_t limit, active;
        GATE_STATS("race.example", 80, limit, active);
        EXPECT_EQ(active, 0);                                                   /* every grant was released */
        EXPECT(limit >= HOSTGATE_MIN && limit <= HOSTGATE_OPEN);
        if (mode == 0) EXPECT_EQ(granted, (long)GATE_THREADS * GATE_ROUNDS);    /* nothing refused: nothing denied */
        else           EXPECT(granted > 0);
    }
}

static void print_summary(void) {
    int total = g_pass + g_fail + g_skip;
    printf("\n%s%s─────────────────────────────%s\n", BOLD, CYAN, RESET);
    printf("%s  Results%s\n", BOLD, RESET);
    printf("%s─────────────────────────────%s\n", CYAN, RESET);
    printf("  %s✔  Passed : %d%s\n", GREEN,  g_pass, RESET);
    printf("  %s✘  Failed : %d%s\n", g_fail ? RED : DIM, g_fail, RESET);
    printf("  %s-  Skipped: %d%s\n", DIM, g_skip, RESET);
    printf("  %s   Total  : %d%s\n", BOLD, total, RESET);
    printf("%s─────────────────────────────%s\n\n", CYAN, RESET);

    if (g_fail > 0) {
        printf("  %s%sFAILED%s\n\n", BOLD, RED, RESET);
    } else {
        printf("  %s%sALL TESTS PASSED%s\n\n", BOLD, GREEN, RESET);
    }
}

int main(void) {
    printf("\n%s%s FluxDM Phase 1 — Unit Tests%s\n", BOLD, CYAN, RESET);

    test_errors();
    test_errors_thread_isolation();
    test_bandwidth();
    test_checksum();
    test_resume();
    test_http_url_parse();
    test_http_headers();
    test_hostgate_slots();
    test_hostgate_refusals();
    test_hostgate_pause_and_pacing();
    test_hostgate_recovery();
    test_hostgate_table();
    test_hostgate_policy();
    test_hostgate_threads();

    print_summary();
    return g_fail > 0 ? 1 : 0;
}
