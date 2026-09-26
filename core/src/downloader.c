/* FluxDM — core/src/downloader.c
 * Each download gets its own epoll fd and pthread.
 * The thread blocks on epoll_wait() and drives segments' state machines.
 */

#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/protocol/http.h"
#include "../include/protocol/https.h"
#include <openssl/err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

/* ── Monotonic clock ──────────────────────────────────────────────────────── */
static uint64_t dl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

/* ── File preparation ─────────────────────────────────────────────────────── */
static int open_output_file(flux_download_t *dl) {
    /* Create or open-for-resume */
    int flags = O_WRONLY | O_CREAT;
    dl->file_fd = open(dl->file_path, flags, 0644);
    if (dl->file_fd < 0) {
        flux_set_syserror("open output file");
        return -1;
    }

    /* Pre-allocate disk space if we know the size — avoids fragmentation */
    if (dl->total_size > 0) {
        if (posix_fallocate(dl->file_fd, 0, (off_t)dl->total_size) != 0) {
            /* Non-fatal: fallback to sparse file */
        }
    }
    return 0;
}

/* ── Segment range calculation ────────────────────────────────────────────── */
static void calculate_ranges(flux_download_t *dl) {
    if (!dl->accept_ranges || dl->total_size == 0 || dl->num_segs == 1) {
        /* Single segment: no range header */
        dl->num_segs             = 1;   /* discard unused segments */
        dl->segs[0]->range_start = 0;
        dl->segs[0]->range_end   = UINT64_MAX;
        return;
    }

    uint64_t chunk = dl->total_size / dl->num_segs;
    for (uint8_t i = 0; i < dl->num_segs; i++) {
        dl->segs[i]->range_start = i * chunk;
        dl->segs[i]->range_end   = (i == dl->num_segs - 1)
                                   ? dl->total_size - 1
                                   : (i + 1) * chunk - 1;
    }
}

/* ── Resume: restore written bytes per segment ────────────────────────────── */
static void apply_resume_state(flux_download_t *dl) {
    for (uint8_t i = 0; i < dl->resume.hdr.num_segments && i < dl->num_segs; i++) {
        flux_resume_seg_t *rs  = &dl->resume.segs[i];
        flux_segment_t    *seg = dl->segs[i];
        if (rs->status == 1) {
            seg->state   = SEG_COMPLETE;
            seg->written = rs->written;
        } else {
            seg->range_start = rs->range_start;
            seg->range_end   = rs->range_end;
            seg->written     = rs->written;
        }
    }
    /* Recalculate downloaded_bytes from resumed state */
    dl->downloaded_bytes = 0;
    for (uint8_t i = 0; i < dl->num_segs; i++)
        dl->downloaded_bytes += dl->segs[i]->written;
}

/* ── Probe (HEAD request, with GET+Range fallback) ────────────────────────── */

/* Result of a single probe attempt (either HEAD or a ranged GET). */
struct probe_result {
    int      status;
    uint64_t content_length;
    int      accept_ranges;
};

/* Opens a fresh connection, performs TLS if needed, sends one request
 * (method + optional extra header line), and parses the response status
 * line + Content-Length + Accept-Ranges. Always uses a brand-new
 * connection — a failed or closed attempt can't be reused for a retry.
 * Returns 0 with *out filled in on success (any HTTP status received),
 * -1 on failure (DNS/connect/TLS/network-level — flux_set_error already
 * called). A non-2xx/206 status is NOT a failure at this layer; the
 * caller decides whether the status itself is acceptable. */
static int probe_send_and_parse(const http_url_t *url_parts, int use_tls,
                                 const char *method, const char *extra_header,
                                 struct probe_result *out) {
    memset(out, 0, sizeof(*out));

    struct addrinfo hints = {0}, *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", url_parts->port);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(url_parts->host, port_str, &hints, &res);
    if (rc != 0) {
        flux_set_error(FLUX_ERR_DNS, "probe getaddrinfo(%s): %s",
                       url_parts->host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        flux_set_error(FLUX_ERR_CONNECT, "probe: cannot connect to %s:%u",
                       url_parts->host, url_parts->port);
        return -1;
    }

    /* ── TLS handshake (blocking) for https:// targets ────────────────────── */
    SSL *ssl = NULL;
    if (use_tls) {
        SSL_CTX *ctx = flux_tls_get_global_ctx();
        if (!ctx) {
            close(fd);
            flux_set_error(FLUX_ERR_TLS_INIT, "probe: TLS not initialised");
            return -1;
        }
        ssl = SSL_new(ctx);
        if (!ssl) {
            close(fd);
            flux_set_error(FLUX_ERR_TLS_INIT, "probe: SSL_new failed");
            return -1;
        }
        SSL_set_tlsext_host_name(ssl, url_parts->host);   /* SNI */
        SSL_set1_host(ssl, url_parts->host);              /* hostname verification */
        SSL_set_fd(ssl, fd);
        SSL_set_connect_state(ssl);

        /* fd is a plain blocking socket, so SSL_connect() performs the
         * entire handshake in one blocking call — no WANT_READ/WANT_WRITE
         * retry loop needed here, unlike the non-blocking segment path. */
        if (SSL_connect(ssl) != 1) {
            char errbuf[256];
            ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
            SSL_free(ssl);
            close(fd);
            flux_set_error(FLUX_ERR_TLS_HNDSHK, "probe: TLS handshake failed: %s", errbuf);
            return -1;
        }
    }

    /* Build and send the request (via TLS if applicable, else plaintext) */
    char req[2048];
    int req_len = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n",
        method, url_parts->path, url_parts->host,
        extra_header ? extra_header : "");

    if (use_tls) {
        if (SSL_write(ssl, req, req_len) <= 0) {
            SSL_free(ssl); close(fd);
            flux_set_error(FLUX_ERR_NETWORK, "probe: %s send failed (TLS)", method);
            return -1;
        }
    } else {
        if (send(fd, req, (size_t)req_len, MSG_NOSIGNAL) < req_len) {
            close(fd);
            flux_set_error(FLUX_ERR_NETWORK, "probe: %s send failed", method);
            return -1;
        }
    }

    /* Accumulate the response into a local buffer until \r\n\r\n or EOF.
     * Some servers (this one included) simply close the connection with
     * zero bytes sent for methods they don't support — that shows up
     * here as header_done staying 0 with buf_len also 0, which the
     * caller treats as "this method isn't supported, try the fallback"
     * rather than a hard network error. */
    char buf[8192] = {0};
    size_t buf_len = 0;
    int header_done = 0;

    while (buf_len < sizeof(buf) - 1) {
        ssize_t n = use_tls
            ? SSL_read(ssl, buf + buf_len, (int)(sizeof(buf) - 1 - buf_len))
            : recv(fd, buf + buf_len, sizeof(buf) - 1 - buf_len, 0);
        if (n > 0) {
            buf_len += (size_t)n;
            for (size_t i = 0; i + 3 < buf_len; i++) {
                if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
                    header_done = 1; break;
                }
            }
            if (header_done) break;
        } else if (n == 0) {
            header_done = (buf_len > 0);
            break;
        } else {
            break;
        }
    }

    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    close(fd);

    if (!header_done || buf_len == 0) {
        flux_set_error(FLUX_ERR_HTTP_PARSE,
                       "probe: %s got no response (server likely doesn't support it)",
                       method);
        return -1;
    }

    /* Parse the status line manually */
    char *sp = strchr(buf, ' ');
    out->status = sp ? atoi(sp + 1) : 0;

    /* Extract Content-Length. For a 200 response this IS the total file
     * size. For a 206 response to our Range: bytes=0-0 probe, this is
     * misleadingly small — it describes the size of the partial body
     * being sent (1 byte), NOT the total resource size. The real total
     * for a 206 lives in Content-Range instead, parsed below, which
     * takes priority whenever both are present. */
    char *cl = strcasestr(buf, "\r\nContent-Length:");
    if (cl) {
        cl += strlen("\r\nContent-Length:");
        while (*cl == ' ') cl++;
        out->content_length = (uint64_t)strtoull(cl, NULL, 10);
    }

    /* Extract total size from Content-Range: bytes 0-0/104857600
     * The number after the '/' is the true total — this is what a 206
     * response actually tells us the full resource size is. */
    char *cr = strcasestr(buf, "\r\nContent-Range:");
    if (cr) {
        char *slash = strchr(cr, '/');
        if (slash) {
            slash++;
            while (*slash == ' ') slash++;
            uint64_t total = (uint64_t)strtoull(slash, NULL, 10);
            if (total > 0) out->content_length = total;
        }
    }

    /* Check Accept-Ranges */
    char *ar = strcasestr(buf, "\r\nAccept-Ranges:");
    if (ar) {
        ar += strlen("\r\nAccept-Ranges:");
        while (*ar == ' ') ar++;
        out->accept_ranges = (strncmp(ar, "bytes", 5) == 0) ? 1 : 0;
    }

    /* A 206 to our Range: bytes=0-0 request is itself proof of range
     * support, regardless of whether Accept-Ranges was also present. */
    if (out->status == 206) out->accept_ranges = 1;

    return 0;
}

static int probe_url(flux_download_t *dl) {
    /* Uses a dedicated blocking socket per attempt — simpler and more
     * reliable than the epoll/non-blocking path for a synchronous probe.
     *
     * Tries HEAD first. Many servers handle it fine. But some — this
     * project's own test case included, a real Hetzner speed-test
     * endpoint confirmed independently via `curl -v --http1.1 -I`
     * getting "Empty reply from server" — simply don't answer HEAD at
     * all and drop the connection. When that happens, fall back to a
     * ranged GET (Range: bytes=0-0), which virtually every real server
     * answers correctly: a 206 confirms range support and gives the
     * total size via Content-Range/Content-Length, a 200 means the
     * server ignored the range (no range support), either way we get
     * the metadata we need without pulling the whole file. */
    http_url_t url_parts;
    if (http_url_parse(dl->url, &url_parts) < 0) return -1;

    int use_tls = (strcmp(url_parts.scheme, "https") == 0);

    struct probe_result result;
    int rc = probe_send_and_parse(&url_parts, use_tls, "HEAD", NULL, &result);

    if (rc < 0 || (result.status != 200 && result.status != 206)) {
        /* HEAD failed outright, or the server answered with something
         * other than success — fall back to a ranged GET. */
        struct probe_result fallback;
        int frc = probe_send_and_parse(&url_parts, use_tls, "GET",
                                       "Range: bytes=0-0\r\n", &fallback);
        if (frc < 0) {
            /* The GET attempt's error is more informative than HEAD's
             * for the caller — it already called flux_set_error(). */
            return -1;
        }
        if (fallback.status != 200 && fallback.status != 206) {
            flux_set_error(http_status_to_flux_error(fallback.status),
                           "probe: HTTP %d (HEAD unsupported, GET fallback also rejected)",
                           fallback.status);
            return -1;
        }
        result = fallback;
    }

    dl->total_size    = result.content_length;
    dl->accept_ranges = result.accept_ranges;
    return 0;
}

/* ── Save resume state ────────────────────────────────────────────────────── */
static void save_resume(flux_download_t *dl) {
    dl->resume.hdr.magic        = FLUX_RESUME_MAGIC;
    dl->resume.hdr.version      = FLUX_RESUME_VER;
    dl->resume.hdr.download_id  = dl->id;
    dl->resume.hdr.num_segments = dl->num_segs;
    dl->resume.hdr.total_size   = dl->total_size;
    dl->resume.hdr.checksum_type = dl->cksum_type;
    strncpy(dl->resume.url,  dl->url,       FLUX_RESUME_MAX_URL - 1);
    strncpy(dl->resume.path, dl->file_path, FLUX_RESUME_MAX_URL - 1);

    for (uint8_t i = 0; i < dl->num_segs; i++) {
        flux_segment_t    *seg = dl->segs[i];
        flux_resume_seg_t *rs  = &dl->resume.segs[i];
        rs->range_start = seg->range_start;
        rs->range_end   = seg->range_end;
        rs->written     = seg->written;
        rs->status      = (seg->state == SEG_COMPLETE) ? 1 : 0;
    }
    flux_resume_save(&dl->resume, dl->part_path);
}

/* ── Progress callback (called from segment.c flush) ─────────────────────── */
void flux_dl_on_progress(flux_download_t *dl, uint64_t bytes) {
    __atomic_add_fetch(&dl->downloaded_bytes, bytes, __ATOMIC_RELAXED);

    uint64_t now = dl_now_ms();
    uint64_t prev = dl->last_progress_ms;
    if (now - prev >= FLUX_PROGRESS_INTERVAL_MS) {
        dl->last_progress_ms = now;
        if (dl->on_event) dl->on_event(dl, FLUX_RSP_PROGRESS);
    }
}

/* ── Bandwidth proxy (called from segment.c) ──────────────────────────────── */
int flux_dl_bw_consume(flux_download_t *dl, size_t bytes) {
    uint32_t wait_ms = 0;
    /* Try per-download limit first */
    if (dl->bw_bucket.rate_bps > 0) {
        if (flux_bw_consume(&dl->bw_bucket, bytes, &wait_ms) < 0) return -1;
    }
    /* Then global limit */
    if (dl->global_bucket && dl->global_bucket->rate_bps > 0) {
        if (flux_bw_consume(dl->global_bucket, bytes, &wait_ms) < 0) return -1;
    }
    return 0;
}

/* ── epoll thread ─────────────────────────────────────────────────────────── */
void *flux_dl_thread(void *arg) {
    flux_download_t     *dl     = arg;
    struct epoll_event   events[FLUX_EPOLL_MAX_EVENTS];
    uint8_t              done   = 0;

    while (!done) {
        int nfds = epoll_wait(dl->epoll_fd, events, FLUX_EPOLL_MAX_EVENTS, 500);

        if (nfds < 0) {
            if (errno == EINTR) continue;
            flux_set_syserror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            struct epoll_event *ev  = &events[i];

            /* Cancel pipe */
            if (ev->data.fd == dl->pipe_cancel[0]) {
                dl->state = DL_CANCELLED;
                done = 1;
                break;
            }

            /* Timer fd (bandwidth resume / retry wakeup) */
            if (ev->data.fd == dl->timer_fd) {
                uint64_t exp;
                ssize_t _tr = read(dl->timer_fd, &exp, sizeof(exp)); (void)_tr;
                /* Re-arm paused/retrying segments */
                for (uint8_t s = 0; s < dl->num_segs; s++) {
                    flux_segment_t *seg = dl->segs[s];
                    if (seg->state == SEG_PAUSED)   flux_seg_resume(seg);
                    if (seg->state == SEG_RETRYING)  flux_seg_start(seg, dl->epoll_fd);
                }
                continue;
            }

            /* Segment event */
            flux_segment_t *seg = ev->data.ptr;
            if (!seg) continue;

            int rc = flux_seg_drive(seg, ev->events);
            if (rc == 1) {
                /* Segment complete */
                dl->num_segs_active--;
            } else if (rc < 0) {
                dl->state = DL_ERROR;
                if (dl->on_event) dl->on_event(dl, FLUX_RSP_ERROR);
                done = 1;
                break;
            }
        }

        /* Periodic: save resume state every ~2 seconds */
        static uint64_t last_save = 0;
        uint64_t now = dl_now_ms();
        if (now - last_save > 2000) {
            save_resume(dl);
            last_save = now;
        }

        /* Check if all segments finished */
        uint8_t complete = 1;
        for (uint8_t s = 0; s < dl->num_segs; s++) {
            if (dl->segs[s]->state != SEG_COMPLETE) { complete = 0; break; }
        }

        if (complete && dl->state == DL_RUNNING) {
            dl->state = DL_COMPLETE;

            /* Verify checksum */
            if (dl->cksum_type != FLUX_CKSUM_NONE && dl->cksum) {
                int ok = flux_cksum_verify(dl->cksum,
                                           dl->cksum_expected,
                                           dl->cksum_type == FLUX_CKSUM_MD5 ? 16 : 32);
                if (ok != 1) {
                    flux_set_error(FLUX_ERR_CHECKSUM_, "Checksum mismatch");
                    dl->state = DL_ERROR;
                    if (dl->on_event) dl->on_event(dl, FLUX_RSP_ERROR);
                    done = 1;
                    break;
                }
            }

            fsync(dl->file_fd);
            flux_resume_delete(dl->part_path);
            if (dl->on_event) dl->on_event(dl, FLUX_RSP_COMPLETE);
            done = 1;
        }
    }

    /* Cleanup */
    for (uint8_t s = 0; s < dl->num_segs; s++) {
        if (dl->segs[s]) flux_seg_flush(dl->segs[s]);
    }
    if (dl->file_fd >= 0) { close(dl->file_fd); dl->file_fd = -1; }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int flux_registry_init(flux_registry_t *reg) {
    memset(reg, 0, sizeof(*reg));
    pthread_mutex_init(&reg->lock, NULL);
    flux_bw_init(&reg->global_bw, 0);   /* unlimited by default */
    return flux_tls_global_init();
}

void flux_registry_destroy(flux_registry_t *reg) {
    for (int i = 0; i < 4096; i++) {
        if (reg->downloads[i]) flux_dl_cancel(reg->downloads[i]);
    }
    flux_bw_destroy(&reg->global_bw);
    pthread_mutex_destroy(&reg->lock);
    flux_tls_global_cleanup();
}

flux_download_t *flux_dl_start(flux_registry_t  *reg,
                               uint32_t          id,
                               const char       *url,
                               const char       *dest_path,
                               uint8_t           num_segs,
                               uint64_t          bw_limit,
                               flux_cksum_type_t cksum_type,
                               const uint8_t    *cksum_expected,
                               void (*on_event)(flux_download_t *, flux_msg_type_t)) {
    if (num_segs == 0 || num_segs > FLUX_DL_MAX_SEGMENTS) {
        flux_set_error(FLUX_ERR_BAD_STATE, "invalid segment count %u", num_segs);
        return NULL;
    }

    flux_download_t *dl = calloc(1, sizeof(*dl));
    if (!dl) { flux_set_error(FLUX_ERR_NOMEM, "dl_start: calloc"); return NULL; }

    dl->id        = id;
    dl->state     = DL_PROBING;
    dl->num_segs  = num_segs;
    dl->on_event  = on_event;
    dl->file_fd   = -1;

    strncpy(dl->url,       url,       FLUX_MAX_URL  - 1);
    strncpy(dl->file_path, dest_path, FLUX_MAX_PATH - 1);
    flux_resume_path(dest_path, dl->part_path, sizeof(dl->part_path));

    /* Parse URL */
    http_url_t up;
    if (http_url_parse(url, &up) < 0) goto fail;
    strncpy(dl->host, up.host, sizeof(dl->host) - 1);
    strncpy(dl->url_path, up.path, sizeof(dl->url_path) - 1);
    dl->port    = up.port;
    dl->use_tls = (strcmp(up.scheme, "https") == 0);

    /* Bandwidth */
    flux_bw_init(&dl->bw_bucket, bw_limit);
    dl->global_bucket = &reg->global_bw;

    /* Checksum */
    dl->cksum_type = cksum_type;
    if (cksum_type != FLUX_CKSUM_NONE) {
        dl->cksum = flux_cksum_new(cksum_type);
        if (cksum_expected)
            memcpy(dl->cksum_expected, cksum_expected, FLUX_CKSUM_MAX_LEN);
    }

    /* Connection pool */
    flux_pool_init(&dl->conn_pool);
    pthread_mutex_init(&dl->lock, NULL);

    /* Cancel pipe */
    if (pipe2(dl->pipe_cancel, O_NONBLOCK) < 0) {
        flux_set_syserror("pipe2"); goto fail;
    }

    /* epoll + timerfd */
    dl->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (dl->epoll_fd < 0) { flux_set_syserror("epoll_create1"); goto fail; }

    dl->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (dl->timer_fd < 0) { flux_set_syserror("timerfd_create"); goto fail; }

    struct epoll_event ev;
    ev.events   = EPOLLIN;
    ev.data.fd  = dl->pipe_cancel[0];
    epoll_ctl(dl->epoll_fd, EPOLL_CTL_ADD, dl->pipe_cancel[0], &ev);

    ev.data.fd  = dl->timer_fd;
    epoll_ctl(dl->epoll_fd, EPOLL_CTL_ADD, dl->timer_fd, &ev);

    /* Try to load resume state */
    int has_resume = (flux_resume_load(&dl->resume, dl->part_path) == 0);

    /* Probe the URL to determine size and range support */
    if (probe_url(dl) < 0) goto fail;

    /* Allocate segments */
    for (uint8_t s = 0; s < num_segs; s++) {
        dl->segs[s] = flux_seg_new(dl, s, 0, UINT64_MAX);
        if (!dl->segs[s]) goto fail;
        dl->segs[s]->file_fd = dl->file_fd;   /* assigned after open */
    }

    calculate_ranges(dl);
    if (has_resume) apply_resume_state(dl);

    /* Open/create output file */
    if (open_output_file(dl) < 0) goto fail;
    for (uint8_t s = 0; s < num_segs; s++)
        dl->segs[s]->file_fd = dl->file_fd;

    /* Start all non-complete segments */
    dl->state = DL_RUNNING;
    for (uint8_t s = 0; s < num_segs; s++) {
        if (dl->segs[s]->state == SEG_COMPLETE) continue;
        if (flux_seg_start(dl->segs[s], dl->epoll_fd) < 0) goto fail;
        dl->num_segs_active++;
    }

    /* Register in registry */
    pthread_mutex_lock(&reg->lock);
    if (id < 4096) reg->downloads[id] = dl;
    reg->count++;
    pthread_mutex_unlock(&reg->lock);

    /* Spawn epoll thread */
    if (pthread_create(&dl->thread, NULL, flux_dl_thread, dl) != 0) {
        flux_set_syserror("pthread_create");
        goto fail;
    }

    return dl;

fail:
    flux_dl_cancel(dl);
    return NULL;
}

int flux_dl_pause(flux_download_t *dl) {
    if (dl->state != DL_RUNNING) return 0;
    dl->state = DL_PAUSED;
    for (uint8_t s = 0; s < dl->num_segs; s++)
        flux_seg_pause(dl->segs[s]);
    save_resume(dl);
    return 0;
}

int flux_dl_resume(flux_download_t *dl) {
    if (dl->state != DL_PAUSED) return 0;
    dl->state = DL_RUNNING;
    for (uint8_t s = 0; s < dl->num_segs; s++)
        if (dl->segs[s]->state == SEG_PAUSED)
            flux_seg_resume(dl->segs[s]);
    return 0;
}

int flux_dl_cancel(flux_download_t *dl) {
    if (!dl) return 0;
    /* Signal the epoll thread to stop */
    if (dl->pipe_cancel[1] >= 0) {
        ssize_t _wr = write(dl->pipe_cancel[1], "x", 1); (void)_wr;
        pthread_join(dl->thread, NULL);
    }
    for (uint8_t s = 0; s < dl->num_segs; s++) flux_seg_free(dl->segs[s]);
    flux_pool_purge(&dl->conn_pool);
    if (dl->cksum) flux_cksum_free(dl->cksum);
    flux_bw_destroy(&dl->bw_bucket);
    if (dl->epoll_fd  >= 0) close(dl->epoll_fd);
    if (dl->timer_fd  >= 0) close(dl->timer_fd);
    if (dl->pipe_cancel[0] >= 0) close(dl->pipe_cancel[0]);
    if (dl->pipe_cancel[1] >= 0) close(dl->pipe_cancel[1]);
    if (dl->file_fd >= 0) close(dl->file_fd);
    pthread_mutex_destroy(&dl->lock);
    free(dl);
    return 0;
}

flux_download_t *flux_registry_find(flux_registry_t *reg, uint32_t id) {
    if (id >= 4096) return NULL;
    pthread_mutex_lock(&reg->lock);
    flux_download_t *dl = reg->downloads[id];
    pthread_mutex_unlock(&reg->lock);
    return dl;
}
