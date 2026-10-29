/* FluxDM — core/src/ipc/socket_server.c
 * See socket_server.h for the threading and ordering rules.
 */

#include "socket_server.h"
#include "sock_path.h"
#include "../../include/errors.h"
#include "../../include/util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The wire layout is a contract with the Java daemon: break it at compile time,
 * not in production. */
_Static_assert(sizeof(flux_cmd_start_t)     == 56,  "CMD_START fixed part is 56 bytes");
_Static_assert(sizeof(flux_cmd_set_limit_t) == 8,   "CMD_SET_LIMIT is 8 bytes");
_Static_assert(sizeof(flux_rsp_error_t)     == 128, "RSP_ERROR is 128 bytes");
_Static_assert(sizeof(flux_rsp_progress_t)  == 32,  "RSP_PROGRESS is 32 bytes");
_Static_assert(sizeof(flux_rsp_complete_t)  == 52,  "RSP_COMPLETE is 52 bytes");
_Static_assert(sizeof(flux_rsp_status_t)    == 32,  "RSP_STATUS is 32 bytes");

#define ALOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define ASTORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#define TAG_LISTEN    0u               /* epoll tags: clients use slot index + 1 */
#define TAG_WAKE      UINT32_MAX
#define OUT_KEEP_CAP  (64u * 1024u)    /* an idle output queue gives back anything larger */

/* The one server of this process, for the event callback (which has no context
 * argument). Set while the loop runs. */
static flux_ipc_server_t *g_server;

/* ═══ Error mapping ═══════════════════════════════════════════════════════ */

uint8_t flux_ipc_wire_error(flux_error_t code) {
    switch (code) {
    case FLUX_OK:                 return FLUX_WIRE_ERR_NONE;
    case FLUX_ERR_BAD_URL:        return FLUX_WIRE_ERR_INVALID_URL;
    case FLUX_ERR_TIMEOUT_:       return FLUX_WIRE_ERR_TIMEOUT;
    case FLUX_ERR_DISK_OPEN:
    case FLUX_ERR_DISK_WRITE:
    case FLUX_ERR_DISK_READ:
    case FLUX_ERR_DISK_SEEK:
    case FLUX_ERR_DISK_SPACE:     return FLUX_WIRE_ERR_DISK;
    case FLUX_ERR_TLS_INIT:
    case FLUX_ERR_TLS_HNDSHK:
    case FLUX_ERR_TLS_IO:
    case FLUX_ERR_TLS_CERT:       return FLUX_WIRE_ERR_TLS;
    case FLUX_ERR_NOT_FOUND_:     return FLUX_WIRE_ERR_NOT_FOUND;
    case FLUX_ERR_FORBIDDEN:      return FLUX_WIRE_ERR_FORBIDDEN;
    case FLUX_ERR_SERVER:
    case FLUX_ERR_HTTP_STATUS:    return FLUX_WIRE_ERR_SERVER;
    case FLUX_ERR_HTTP_RANGE:
    case FLUX_ERR_REMOTE_CHANGED: return FLUX_WIRE_ERR_RANGE;
    case FLUX_ERR_CHECKSUM_:      return FLUX_WIRE_ERR_CHECKSUM;
    case FLUX_ERR_SYS:
    case FLUX_ERR_CONNECT:
    case FLUX_ERR_DNS:
    case FLUX_ERR_BADADDR:
    case FLUX_ERR_RESET:
    case FLUX_ERR_PIPE:
    case FLUX_ERR_NETWORK:
    case FLUX_ERR_HTTP_PARSE:
    case FLUX_ERR_HTTP_REDIR:     return FLUX_WIRE_ERR_NETWORK;
    default:                      return FLUX_WIRE_ERR_INTERNAL;
    }
}

/* ═══ Output queue ════════════════════════════════════════════════════════ */

/* (Un)register interest in EPOLLOUT. out_lock held. */
static void client_arm_locked(flux_ipc_client_t *c, int want) {
    if (c->out_armed == want || c->fd < 0) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN | (want ? (uint32_t)EPOLLOUT : 0u);
    ev.data.u32 = (uint32_t)(c - c->srv->clients) + 1u;
    if (epoll_ctl(c->srv->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == 0) c->out_armed = want;
}

/* Push as much queued output as the socket takes right now. out_lock held. */
static void client_flush_locked(flux_ipc_client_t *c) {
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { c->out_off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            client_arm_locked(c, 1);                 /* resume from the IPC loop on EPOLLOUT */
            return;
        }
        ASTORE(&c->closing, 1);                      /* the peer is gone */
        return;
    }
    c->out_off = c->out_len = 0;
    client_arm_locked(c, 0);
    if (c->out_cap > OUT_KEEP_CAP) { free(c->out); c->out = NULL; c->out_cap = 0; }
}

/* Room for `extra` more bytes behind the pending ones. out_lock held. */
static int out_reserve_locked(flux_ipc_client_t *c, size_t extra) {
    if (c->out_off == c->out_len) c->out_off = c->out_len = 0;
    if (c->out_cap - c->out_len >= extra) return 0;

    if (c->out_off > 0) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off  = 0;
        if (c->out_cap - c->out_len >= extra) return 0;
    }
    size_t ncap = c->out_cap ? c->out_cap : 4096u;
    while (ncap - c->out_len < extra) ncap *= 2;
    uint8_t *nb = realloc(c->out, ncap);
    if (!nb) return -1;
    c->out     = nb;
    c->out_cap = ncap;
    return 0;
}

/* Append one whole frame to a client's queue and try to send it. Thread-safe.
 * `droppable` frames (progress) are skipped when the client is behind.
 * Returns -1 if the client is, or has just become, unusable. */
static int client_send(flux_ipc_client_t *c, flux_msg_type_t type, uint16_t flags, uint32_t id,
                       const void *payload, uint32_t plen, int droppable) {
    size_t flen = sizeof(flux_header_t) + plen;
    int    rc   = 0;

    pthread_mutex_lock(&c->out_lock);
    if (c->fd < 0 || ALOAD(&c->closing)) {
        rc = -1;
    } else {
        size_t pending = c->out_len - c->out_off;
        if (droppable && pending > FLUX_IPC_OUT_SOFT) {
            /* behind: the next progress frame supersedes this one */
        } else if (pending + flen > FLUX_IPC_OUT_HARD || out_reserve_locked(c, flen) < 0) {
            ASTORE(&c->closing, 1);                  /* hopelessly behind (or out of memory) */
            rc = -1;
        } else {
            flux_frame_encode(c->out + c->out_len, flen, type, flags, id, payload, plen);
            c->out_len += flen;
            client_flush_locked(c);
        }
    }
    pthread_mutex_unlock(&c->out_lock);
    return rc;
}

/* An event for everyone. Called from worker threads; never blocks. */
static void broadcast(flux_ipc_server_t *srv, flux_msg_type_t type, uint32_t id,
                      const void *payload, uint32_t plen, int droppable) {
    pthread_mutex_lock(&srv->clients_lock);
    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd >= 0)
            client_send(&srv->clients[i], type, FLUX_FLAG_EVENT, id, payload, plen, droppable);
    pthread_mutex_unlock(&srv->clients_lock);
}

/* ═══ Replies ═════════════════════════════════════════════════════════════ */

static void reply_ok(flux_ipc_client_t *c, uint32_t id) {
    client_send(c, FLUX_RSP_OK, 0, id, NULL, 0, 0);
}

/* Fill the fixed-size message field of an RSP_ERROR, cutting an over-long text
 * at a UTF-8 character boundary rather than in the middle of a sequence. */
static void error_text(flux_rsp_error_t *e, const char *src) {
    const size_t cap = sizeof(e->message);
    if (!src) src = "";
    size_t n = strnlen(src, cap - 1u);
    if (src[n] != '\0')                                       /* truncated */
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) n--;
    memset(e->message, 0, cap);
    memcpy(e->message, src, n);
}

static void reply_error(flux_ipc_client_t *c, uint32_t id, uint8_t wire, const char *msg) {
    flux_rsp_error_t e;
    memset(&e, 0, sizeof(e));
    e.code = wire;
    error_text(&e, msg);
    client_send(c, FLUX_RSP_ERROR, 0, id, &e, sizeof(e), 0);
}

/* Reply with whatever the last failing core call recorded. */
static void reply_last_error(flux_ipc_client_t *c, uint32_t id) {
    reply_error(c, id, flux_ipc_wire_error(flux_last_error()), flux_last_error_str());
}

static void reply_status(flux_ipc_client_t *c, const flux_download_t *dl) {
    flux_rsp_status_t st;
    flux_dl_snapshot(dl, &st);
    client_send(c, FLUX_RSP_STATUS, 0, dl->id, &st, sizeof(st), 0);
}

/* ═══ Commands ════════════════════════════════════════════════════════════ */

/* Fixed struct, then u16 url_len + url, then u16 path_len + path. Every length
 * is checked against the frame; embedded NULs are refused rather than silently
 * truncating the string. */
static int parse_start(const uint8_t *pl, uint32_t len, flux_cmd_start_t *cmd,
                       char *url, char *path, const char **why) {
    if (len < sizeof(*cmd) + 2u) { *why = "CMD_START payload too short"; return -1; }
    memcpy(cmd, pl, sizeof(*cmd));
    size_t pos = sizeof(*cmd);

    size_t ulen = (size_t)pl[pos] | ((size_t)pl[pos + 1] << 8);
    pos += 2;
    if (ulen == 0 || ulen >= FLUX_MAX_URL || pos + ulen > len) { *why = "CMD_START has a bad URL length"; return -1; }
    if (memchr(pl + pos, 0, ulen))                             { *why = "CMD_START URL contains a NUL byte"; return -1; }
    memcpy(url, pl + pos, ulen);
    url[ulen] = '\0';
    pos += ulen;

    if (pos + 2 > len) { *why = "CMD_START is missing the destination path"; return -1; }
    size_t plen = (size_t)pl[pos] | ((size_t)pl[pos + 1] << 8);
    pos += 2;
    if (plen == 0 || plen >= FLUX_MAX_PATH || pos + plen > len) { *why = "CMD_START has a bad path length"; return -1; }
    if (memchr(pl + pos, 0, plen))                              { *why = "CMD_START path contains a NUL byte"; return -1; }
    memcpy(path, pl + pos, plen);
    path[plen] = '\0';
    return 0;
}

static void handle_start(flux_ipc_server_t *srv, flux_ipc_client_t *c, uint32_t id,
                         const uint8_t *pl, uint32_t len) {
    flux_cmd_start_t cmd;
    char             url[FLUX_MAX_URL];
    char             path[FLUX_MAX_PATH];
    const char      *why = NULL;

    if (parse_start(pl, len, &cmd, url, path, &why) < 0) {
        reply_error(c, id, FLUX_WIRE_ERR_INTERNAL, why);
        return;
    }

    flux_download_t *dl = flux_dl_start(srv->registry, id, url, path,
                                        (uint8_t)(cmd.segments > 255u ? 255u : cmd.segments),
                                        cmd.bandwidth_limit,
                                        (flux_cksum_type_t)cmd.checksum_type, cmd.checksum,
                                        flux_ipc_on_dl_event);
    if (!dl) { reply_last_error(c, id); return; }

    /* Acknowledge first: the worker's events must never overtake the OK. */
    reply_ok(c, id);
    flux_dl_launch(dl);
}

typedef struct { flux_ipc_client_t *client; } status_all_ctx_t;

static void status_all_cb(flux_download_t *dl, void *ctx) {
    reply_status(((status_all_ctx_t *)ctx)->client, dl);
}

static void dispatch(flux_ipc_server_t *srv, flux_ipc_client_t *c,
                     const flux_header_t *h, const uint8_t *pl) {
    flux_registry_t *reg = srv->registry;
    uint32_t         id  = h->id;
    flux_download_t *dl;

    flux_log(2, "client %u: command 0x%02x id %u (%u payload bytes)", c->serial,
             (unsigned)h->type, id, (unsigned)h->payload_len);

    switch ((flux_msg_type_t)h->type) {

    case FLUX_CMD_START:
        handle_start(srv, c, id, pl, h->payload_len);
        break;

    case FLUX_CMD_PAUSE:
        if (!(dl = flux_registry_find(reg, id))) { reply_error(c, id, FLUX_WIRE_ERR_NOT_FOUND, "no such download"); break; }
        if (flux_dl_pause(dl) < 0) reply_last_error(c, id); else reply_ok(c, id);
        break;

    case FLUX_CMD_RESUME:
        if (!(dl = flux_registry_find(reg, id))) { reply_error(c, id, FLUX_WIRE_ERR_NOT_FOUND, "no such download"); break; }
        if (flux_dl_resume(dl) < 0) reply_last_error(c, id); else reply_ok(c, id);
        break;

    case FLUX_CMD_CANCEL:
        /* Idempotent: cancelling something that is already gone is success. */
        if ((dl = flux_registry_find(reg, id))) flux_dl_cancel(reg, dl);
        reply_ok(c, id);
        break;

    case FLUX_CMD_SET_LIMIT: {
        flux_cmd_set_limit_t lim;
        if (h->payload_len < sizeof(lim)) { reply_error(c, id, FLUX_WIRE_ERR_INTERNAL, "CMD_SET_LIMIT payload too short"); break; }
        memcpy(&lim, pl, sizeof(lim));
        if (id == 0) {                                        /* id 0 = the daemon-wide limit */
            flux_bw_set_rate(&reg->global_bw, lim.bytes_per_sec);
        } else if ((dl = flux_registry_find(reg, id))) {
            flux_bw_set_rate(&dl->bw_bucket, lim.bytes_per_sec);
        } else {
            reply_error(c, id, FLUX_WIRE_ERR_NOT_FOUND, "no such download");
            break;
        }
        reply_ok(c, id);
        break;
    }

    case FLUX_CMD_STATUS:
        if (!(dl = flux_registry_find(reg, id))) { reply_error(c, id, FLUX_WIRE_ERR_NOT_FOUND, "no such download"); break; }
        reply_status(c, dl);
        break;

    case FLUX_CMD_STATUS_ALL: {
        status_all_ctx_t ctx = { c };
        flux_registry_foreach(reg, status_all_cb, &ctx);
        reply_ok(c, 0);                                       /* the terminator */
        break;
    }

    case FLUX_CMD_SHUTDOWN:
        reply_ok(c, 0);
        ASTORE(&srv->running, 0);
        break;

    default: {
        char msg[48];
        snprintf(msg, sizeof(msg), "unknown command 0x%02x", (unsigned)h->type);
        reply_error(c, id, FLUX_WIRE_ERR_INTERNAL, msg);
        break;
    }
    }
}

/* ═══ Events from the download workers ════════════════════════════════════ */

void flux_ipc_on_dl_event(flux_download_t *dl, flux_msg_type_t event) {
    flux_ipc_server_t *srv = ALOAD(&g_server);
    if (!srv) return;

    switch (event) {

    case FLUX_RSP_PROGRESS: {
        flux_rsp_status_t st;
        flux_dl_snapshot(dl, &st);
        flux_rsp_progress_t p;
        memset(&p, 0, sizeof(p));
        p.id               = st.id;
        p.total_bytes      = st.total_bytes;
        p.downloaded_bytes = st.downloaded_bytes;
        p.speed_bps        = st.speed_bps;
        p.eta_seconds      = st.eta_seconds;
        p.active_segments  = st.active_segments;
        p.status           = st.status;
        broadcast(srv, FLUX_RSP_PROGRESS, dl->id, &p, sizeof(p), 1);
        break;
    }

    case FLUX_RSP_COMPLETE: {
        flux_rsp_complete_t cpl;
        memset(&cpl, 0, sizeof(cpl));
        uint64_t elapsed = dl->finished_ms >= dl->started_ms ? dl->finished_ms - dl->started_ms : 0u;
        cpl.id          = dl->id;
        cpl.total_bytes = __atomic_load_n(&dl->total_size, __ATOMIC_ACQUIRE);
        cpl.elapsed_ms  = (uint32_t)flux_min_u64(elapsed, UINT32_MAX);
        cpl.checksum_ok = dl->cksum_ok;
        memcpy(cpl.checksum, dl->cksum_actual, sizeof(cpl.checksum));
        broadcast(srv, FLUX_RSP_COMPLETE, dl->id, &cpl, sizeof(cpl), 0);
        break;
    }

    case FLUX_RSP_ERROR: {
        flux_rsp_error_t e;
        memset(&e, 0, sizeof(e));
        e.code = flux_ipc_wire_error(dl->err_code);
        error_text(&e, dl->err_msg);
        broadcast(srv, FLUX_RSP_ERROR, dl->id, &e, sizeof(e), 0);
        break;
    }

    default:
        break;
    }
}

/* ═══ Client lifecycle ════════════════════════════════════════════════════ */

/* Claim a slot for a freshly accepted fd and register it with epoll. */
static flux_ipc_client_t *client_alloc(flux_ipc_server_t *srv, int fd) {
    flux_ipc_client_t *found = NULL;

    pthread_mutex_lock(&srv->clients_lock);
    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS && !found; i++) {
        flux_ipc_client_t *c = &srv->clients[i];
        if (c->fd >= 0) continue;

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events   = EPOLLIN;
        ev.data.u32 = (uint32_t)i + 1u;
        if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) break;

        pthread_mutex_lock(&c->out_lock);
        flux_frame_reader_init(&c->rd);
        c->out = NULL;
        c->out_cap = c->out_len = c->out_off = 0;
        c->out_armed = 0;
        c->serial    = ++srv->next_serial;
        ASTORE(&c->closing, 0);
        c->fd = fd;                              /* publishing the fd makes it a broadcast target */
        pthread_mutex_unlock(&c->out_lock);
        found = c;
    }
    pthread_mutex_unlock(&srv->clients_lock);
    return found;
}

static void client_free(flux_ipc_server_t *srv, flux_ipc_client_t *c) {
    pthread_mutex_lock(&srv->clients_lock);      /* no broadcast is mid-flight past this point */
    pthread_mutex_lock(&c->out_lock);
    if (c->fd >= 0) {
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
        flux_log(1, "client %u disconnected", c->serial);
    }
    free(c->out);
    c->out = NULL;
    c->out_cap = c->out_len = c->out_off = 0;
    c->out_armed = 0;
    flux_frame_reader_free(&c->rd);
    pthread_mutex_unlock(&c->out_lock);
    pthread_mutex_unlock(&srv->clients_lock);
}

static void accept_clients(flux_ipc_server_t *srv) {
    for (;;) {
        int fd = accept4(srv->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;                              /* EAGAIN: all accepted */
        }
        flux_ipc_client_t *c = client_alloc(srv, fd);
        if (!c) {
            flux_log(1, "too many clients; refusing a connection");
            close(fd);
            continue;
        }
        flux_log(1, "client %u connected", c->serial);
    }
}

/* Decode and run every complete frame buffered for `c`. 0 if the client was
 * dropped (malformed stream), 1 otherwise. */
static int process_frames(flux_ipc_server_t *srv, flux_ipc_client_t *c) {
    for (;;) {
        flux_header_t  h;
        const uint8_t *pl = NULL;
        frame_status_t st = flux_frame_reader_next(&c->rd, &h, &pl);

        if (st == FRAME_NEED_MORE) return 1;
        if (st == FRAME_BAD) {
            flux_log(1, "client %u sent a malformed frame; disconnecting", c->serial);
            client_free(srv, c);
            return 0;
        }
        dispatch(srv, c, &h, pl);
        if (ALOAD(&c->closing)) { client_free(srv, c); return 0; }
    }
}

static void client_readable(flux_ipc_server_t *srv, flux_ipc_client_t *c) {
    for (int round = 0; round < 16; round++) {  /* fairness: epoll is level-triggered, so leftovers come back */
        uint8_t *dst;
        size_t   space;
        if (flux_frame_reader_reserve(&c->rd, &dst, &space) < 0) { client_free(srv, c); return; }

        ssize_t n = recv(c->fd, dst, space, 0);
        if (n > 0) {
            flux_frame_reader_commit(&c->rd, (size_t)n);
            if (!process_frames(srv, c)) return;
            if ((size_t)n < space) return;       /* short read: the socket is drained */
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        client_free(srv, c);                     /* EOF or a hard error */
        return;
    }
}

/* ═══ Server ══════════════════════════════════════════════════════════════ */

int flux_ipc_server_init(flux_ipc_server_t *srv, flux_registry_t *reg, const char *sock_path) {
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = srv->epoll_fd = srv->wake_fd = -1;
    srv->registry  = reg;
    pthread_mutex_init(&srv->clients_lock, NULL);
    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++) {
        srv->clients[i].srv = srv;
        srv->clients[i].fd  = -1;
        pthread_mutex_init(&srv->clients[i].out_lock, NULL);
    }

    char        fallback[sizeof(srv->sock_path)];
    const char *path = sock_path;
    if (!path || !*path) {
        if (flux_ipc_default_sock_path(fallback, sizeof(fallback)) < 0) return -1;     /* the error is set */
        path = fallback;
    }
    if (strlen(path) >= sizeof(srv->sock_path)) {
        flux_set_error(FLUX_ERR_IPC_BIND, "socket path too long (max %zu characters): %s",
                       sizeof(srv->sock_path) - 1, path);
        return -1;
    }
    snprintf(srv->sock_path, sizeof(srv->sock_path), "%s", path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /* Is the path taken? A live server answers a connect(); a stale file left
     * by a crash refuses it; anything that is not a socket is none of our
     * business and is never deleted. */
    struct stat sb;
    if (lstat(path, &sb) == 0) {
        if (!S_ISSOCK(sb.st_mode)) {
            flux_set_error(FLUX_ERR_IPC_BIND, "%s exists and is not a socket", path);
            return -1;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe >= 0) {
            int alive = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            close(probe);
            if (alive) {
                flux_set_error(FLUX_ERR_IPC_BIND, "another fluxdm-core is already serving %s", path);
                return -1;
            }
        }
        unlink(path);
    }

    srv->wake_fd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->wake_fd < 0 || srv->epoll_fd < 0) { flux_set_syserror("eventfd/epoll_create1"); return -1; }

    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (srv->listen_fd < 0) { flux_set_syserror("socket"); return -1; }

    mode_t old_umask = umask(0177);              /* owner-only from the moment it exists */
    int    rc        = bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    int    bind_errno = errno;
    umask(old_umask);
    if (rc < 0) {
        flux_set_error(FLUX_ERR_IPC_BIND, "bind(%s): %s", path, strerror(bind_errno));
        return -1;
    }
    srv->owns_socket = 1;
    chmod(path, 0600);

    if (listen(srv->listen_fd, FLUX_IPC_BACKLOG) < 0) { flux_set_syserror("listen"); return -1; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.u32 = TAG_LISTEN;
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);
    ev.data.u32 = TAG_WAKE;
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->wake_fd, &ev);

    flux_log(1, "IPC listening on %s", path);
    return 0;
}

void flux_ipc_stop(flux_ipc_server_t *srv) {
    ASTORE(&srv->running, 0);
    if (srv->wake_fd >= 0) {                     /* write(2) is async-signal-safe */
        uint64_t one = 1;
        ssize_t  w   = write(srv->wake_fd, &one, sizeof(one));
        (void)w;
    }
}

int flux_ipc_server_run(flux_ipc_server_t *srv) {
    struct epoll_event evs[FLUX_IPC_MAX_CLIENTS + 8];
    int      rc        = 0;
    uint64_t last_reap = flux_now_ms();

    ASTORE(&srv->running, 1);
    ASTORE(&g_server, srv);

    while (ALOAD(&srv->running)) {
        int n = epoll_wait(srv->epoll_fd, evs, (int)(sizeof(evs) / sizeof(evs[0])), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            flux_set_syserror("epoll_wait");
            rc = -1;
            break;
        }

        for (int i = 0; i < n; i++) {
            uint32_t tag = evs[i].data.u32;
            if (tag == TAG_LISTEN) {
                accept_clients(srv);
            } else if (tag == TAG_WAKE) {
                uint64_t v;
                ssize_t  r = read(srv->wake_fd, &v, sizeof(v));
                (void)r;
            } else if (tag - 1u < FLUX_IPC_MAX_CLIENTS) {
                flux_ipc_client_t *c = &srv->clients[tag - 1u];
                if (c->fd < 0) continue;         /* closed earlier in this batch */
                if (evs[i].events & EPOLLOUT) {
                    pthread_mutex_lock(&c->out_lock);
                    if (c->fd >= 0) client_flush_locked(c);
                    pthread_mutex_unlock(&c->out_lock);
                }
                if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) client_readable(srv, c);
            }
        }

        /* Clients a worker found dead or hopelessly behind. */
        for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++)
            if (srv->clients[i].fd >= 0 && ALOAD(&srv->clients[i].closing))
                client_free(srv, &srv->clients[i]);

        uint64_t now = flux_now_ms();
        if (now - last_reap >= FLUX_IPC_REAP_MS) {
            flux_registry_reap(srv->registry, now);
            last_reap = now;
        }
    }

    ASTORE(&g_server, NULL);
    /* Stop accepting right away: the downloads still have to be wound down, and
     * a successor should not find a listener nobody serves. */
    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    if (srv->owns_socket)    { unlink(srv->sock_path); srv->owns_socket = 0; }
    return rc;
}

void flux_ipc_server_close(flux_ipc_server_t *srv) {
    if (ALOAD(&g_server) == srv) ASTORE(&g_server, NULL);
    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++)
        if (srv->clients[i].fd >= 0) client_free(srv, &srv->clients[i]);

    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    if (srv->owns_socket)    { unlink(srv->sock_path); srv->owns_socket = 0; }
    if (srv->epoll_fd  >= 0) { close(srv->epoll_fd);   srv->epoll_fd  = -1; }
    if (srv->wake_fd   >= 0) { close(srv->wake_fd);    srv->wake_fd   = -1; }

    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++) pthread_mutex_destroy(&srv->clients[i].out_lock);
    pthread_mutex_destroy(&srv->clients_lock);
}
