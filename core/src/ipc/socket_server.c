/* FluxDM — core/src/ipc/socket_server.c
 * Unix domain socket server — epoll-driven, non-blocking.
 * Receives framed commands from the Java daemon, dispatches to the
 * download registry, and pushes progress/complete events back.
 */

#include "socket_server.h"
#include "../../include/errors.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <arpa/inet.h>

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Write exactly len bytes, retrying on EINTR. */
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p   = buf;
    size_t         rem = len;
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return 0;
}

/* Build and send a raw framed message (header + optional payload). */
static int send_frame(int fd, flux_msg_type_t type, uint32_t id,
                      const void *payload, uint32_t payload_len) {
    flux_header_t hdr = {
        .magic       = FLUX_MAGIC,
        .version     = FLUX_PROTO_VER,
        .type        = (uint8_t)type,
        .flags       = 0,
        .id          = id,
        .payload_len = payload_len,
    };
    if (write_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload && payload_len > 0)
        if (write_all(fd, payload, payload_len) < 0) return -1;
    return 0;
}

/* ── Response helpers ─────────────────────────────────────────────────────── */

int flux_ipc_send_ok(int fd, uint32_t id) {
    return send_frame(fd, FLUX_RSP_OK, id, NULL, 0);
}

int flux_ipc_send_error(int fd, uint32_t id,
                        uint8_t code, const char *msg) {
    flux_rsp_error_t e = {0};
    e.code = (uint8_t)code;
    if (msg) strncpy(e.message, msg, sizeof(e.message) - 1);
    return send_frame(fd, FLUX_RSP_ERROR, id, &e, sizeof(e));
}

int flux_ipc_send_progress(int fd, const flux_download_t *dl) {
    flux_rsp_progress_t p = {
        .id               = dl->id,
        .total_bytes      = dl->total_size,
        .downloaded_bytes = __atomic_load_n(&dl->downloaded_bytes, __ATOMIC_RELAXED),
        .speed_bps        = dl->speed_bps,
        .eta_seconds      = (dl->speed_bps > 0 && dl->total_size > 0)
            ? (uint32_t)((dl->total_size - dl->downloaded_bytes) / dl->speed_bps)
            : UINT32_MAX,
        .active_segments  = dl->num_segs_active,
    };
    return send_frame(fd, FLUX_RSP_PROGRESS, dl->id, &p, sizeof(p));
}

int flux_ipc_send_complete(int fd, const flux_download_t *dl) {
    flux_rsp_complete_t c = {
        .id          = dl->id,
        .total_bytes = dl->total_size,
        .checksum_ok = (dl->cksum_type == FLUX_CKSUM_NONE) ? 0 : 1,
    };
    return send_frame(fd, FLUX_RSP_COMPLETE, dl->id, &c, sizeof(c));
}

/* ── Event callback (registered when a download is created) ───────────────── */

/* We store the client fd alongside the download via a small context struct */
typedef struct {
    int     client_fd;
    uint32_t dl_id;
} ipc_dl_ctx_t;

void flux_ipc_on_dl_event(flux_download_t *dl, flux_msg_type_t event) {
    /* The on_event function needs the client fd.
     * We stash it in the upper 32 bits of a pointer-sized field.
     * In practice, downloader.c sets dl->on_event and we recover the
     * client fd from a small side-table keyed by dl->id. For Phase 1
     * simplicity we use a flat array indexed by id. */
    extern int g_client_fd_by_dl[4096];
    int fd = g_client_fd_by_dl[dl->id < 4096 ? dl->id : 0];
    if (fd < 0) return;

    switch (event) {
        case FLUX_RSP_PROGRESS: flux_ipc_send_progress(fd, dl); break;
        case FLUX_RSP_COMPLETE: flux_ipc_send_complete(fd, dl); break;
        case FLUX_RSP_ERROR:
            flux_ipc_send_error(fd, dl->id,
                                (uint8_t)flux_last_error(),
                                flux_last_error_str());
            break;
        default: break;
    }
}

/* ── Message dispatcher ───────────────────────────────────────────────────── */

/* Global side-table: dl_id → client fd (written when CMD_START received) */
int g_client_fd_by_dl[4096];

int flux_ipc_dispatch(flux_ipc_server_t *srv, flux_ipc_client_t *client,
                      const flux_header_t *hdr, const uint8_t *payload) {
    flux_registry_t *reg = srv->registry;
    uint32_t         id  = hdr->id;

    /* Validate magic and version */
    if (hdr->magic != FLUX_MAGIC || hdr->version != FLUX_PROTO_VER) {
        flux_ipc_send_error(client->fd, id,
                            FLUX_ERR_IPC_PROTO, "bad magic/version");
        return -1;
    }

    switch ((flux_msg_type_t)hdr->type) {

    case FLUX_CMD_START: {
        if (hdr->payload_len < sizeof(flux_cmd_start_t)) {
            flux_ipc_send_error(client->fd, id,
                                FLUX_ERR_IPC_MSG, "CMD_START payload too short");
            return -1;
        }
        const flux_cmd_start_t *cmd = (const flux_cmd_start_t *)payload;

        /* Extract variable-length url and path that follow the struct */
        const uint8_t *vp = payload + sizeof(flux_cmd_start_t);
        size_t         rem = hdr->payload_len - sizeof(flux_cmd_start_t);

        if (rem < 2) goto bad_start;
        uint16_t url_len = (uint16_t)(vp[0] | (vp[1] << 8));
        vp += 2; rem -= 2;
        if (rem < url_len) goto bad_start;
        char url[FLUX_MAX_URL] = {0};
        memcpy(url, vp, url_len < FLUX_MAX_URL - 1 ? url_len : FLUX_MAX_URL - 1);
        vp += url_len; rem -= url_len;

        if (rem < 2) goto bad_start;
        uint16_t path_len = (uint16_t)(vp[0] | (vp[1] << 8));
        vp += 2; rem -= 2;
        if (rem < path_len) goto bad_start;
        char path[FLUX_MAX_PATH] = {0};
        memcpy(path, vp, path_len < FLUX_MAX_PATH - 1 ? path_len : FLUX_MAX_PATH - 1);

        /* Register client fd so on_event callbacks reach this connection */
        if (id < 4096) g_client_fd_by_dl[id] = client->fd;

        flux_download_t *dl = flux_dl_start(
            reg, id, url, path,
            (uint8_t)(cmd->segments > 0 ? cmd->segments : 8),
            cmd->bandwidth_limit,
            (flux_cksum_type_t)cmd->checksum_type,
            cmd->checksum,
            flux_ipc_on_dl_event
        );

        if (!dl) {
            flux_ipc_send_error(client->fd, id,
                                (uint8_t)flux_last_error(),
                                flux_last_error_str());
            return -1;
        }
        return flux_ipc_send_ok(client->fd, id);

    bad_start:
        flux_ipc_send_error(client->fd, id,
                            FLUX_ERR_IPC_MSG, "CMD_START malformed payload");
        return -1;
    }

    case FLUX_CMD_PAUSE: {
        flux_download_t *dl = flux_registry_find(reg, id);
        if (!dl) {
            flux_ipc_send_error(client->fd, id, FLUX_ERR_NOT_FOUND_, "not found");
            return -1;
        }
        flux_dl_pause(dl);
        return flux_ipc_send_ok(client->fd, id);
    }

    case FLUX_CMD_RESUME: {
        flux_download_t *dl = flux_registry_find(reg, id);
        if (!dl) {
            flux_ipc_send_error(client->fd, id, FLUX_ERR_NOT_FOUND_, "not found");
            return -1;
        }
        flux_dl_resume(dl);
        return flux_ipc_send_ok(client->fd, id);
    }

    case FLUX_CMD_CANCEL: {
        flux_download_t *dl = flux_registry_find(reg, id);
        if (!dl) return flux_ipc_send_ok(client->fd, id); /* idempotent */
        if (id < 4096) { reg->downloads[id] = NULL; reg->count--; }
        flux_dl_cancel(dl);
        if (id < 4096) g_client_fd_by_dl[id] = -1;
        return flux_ipc_send_ok(client->fd, id);
    }

    case FLUX_CMD_SET_LIMIT: {
        if (hdr->payload_len < sizeof(flux_cmd_set_limit_t)) {
            flux_ipc_send_error(client->fd, id,
                                FLUX_ERR_IPC_MSG, "SET_LIMIT payload too short");
            return -1;
        }
        const flux_cmd_set_limit_t *cmd = (const flux_cmd_set_limit_t *)payload;
        if (id == 0) {
            /* id=0 means global limit */
            flux_bw_set_rate(&reg->global_bw, cmd->bytes_per_sec);
        } else {
            flux_download_t *dl = flux_registry_find(reg, id);
            if (dl) flux_bw_set_rate(&dl->bw_bucket, cmd->bytes_per_sec);
        }
        return flux_ipc_send_ok(client->fd, id);
    }

    case FLUX_CMD_STATUS: {
        flux_download_t *dl = flux_registry_find(reg, id);
        if (!dl) {
            flux_ipc_send_error(client->fd, id, FLUX_ERR_NOT_FOUND_, "not found");
            return -1;
        }
        flux_rsp_status_t st = {
            .id               = dl->id,
            .status           = (uint8_t)dl->state,
            .active_segments  = dl->num_segs_active,
            .total_segments   = dl->num_segs,
            .total_bytes      = dl->total_size,
            .downloaded_bytes = __atomic_load_n(&dl->downloaded_bytes, __ATOMIC_RELAXED),
            .speed_bps        = dl->speed_bps,
        };
        return send_frame(client->fd, FLUX_RSP_STATUS, id, &st, sizeof(st));
    }

    case FLUX_CMD_STATUS_ALL: {
        for (int i = 0; i < 4096; i++) {
            flux_download_t *dl = reg->downloads[i];
            if (!dl) continue;
            flux_rsp_status_t st = {
                .id               = dl->id,
                .status           = (uint8_t)dl->state,
                .active_segments  = dl->num_segs_active,
                .total_segments   = dl->num_segs,
                .total_bytes      = dl->total_size,
                .downloaded_bytes = __atomic_load_n(&dl->downloaded_bytes, __ATOMIC_RELAXED),
                .speed_bps        = dl->speed_bps,
            };
            send_frame(client->fd, FLUX_RSP_STATUS, dl->id, &st, sizeof(st));
        }
        return flux_ipc_send_ok(client->fd, 0);
    }

    case FLUX_CMD_SHUTDOWN:
        flux_ipc_stop(srv);
        return flux_ipc_send_ok(client->fd, 0);

    default:
        flux_ipc_send_error(client->fd, id,
                            FLUX_ERR_IPC_PROTO, "unknown command");
        return -1;
    }
}

/* ── Client I/O: accumulate bytes until a full frame is ready ─────────────── */
static void client_readable(flux_ipc_server_t *srv, flux_ipc_client_t *c) {
    ssize_t n = read(c->fd,
                     c->recv_buf + c->recv_len,
                     sizeof(c->recv_buf) - c->recv_len);
    if (n <= 0) {
        /* Client disconnected */
        close(c->fd);
        c->fd     = -1;
        c->active = 0;
        return;
    }
    c->recv_len += (size_t)n;

    /* Process as many complete frames as are buffered */
    while (c->recv_len >= sizeof(flux_header_t)) {
        const flux_header_t *hdr = (const flux_header_t *)c->recv_buf;
        size_t frame_size = sizeof(flux_header_t) + hdr->payload_len;

        if (hdr->payload_len > FLUX_MAX_PAYLOAD) {
            /* Bogus length — close the connection */
            close(c->fd); c->fd = -1; c->active = 0;
            return;
        }
        if (c->recv_len < frame_size) break;   /* partial frame, wait */

        const uint8_t *payload = c->recv_buf + sizeof(flux_header_t);
        flux_ipc_dispatch(srv, c, hdr, payload);

        /* Consume the frame */
        size_t remaining = c->recv_len - frame_size;
        if (remaining > 0)
            memmove(c->recv_buf, c->recv_buf + frame_size, remaining);
        c->recv_len = remaining;
    }
}

/* ── Server main loop ─────────────────────────────────────────────────────── */
int flux_ipc_server_run(flux_ipc_server_t *srv, flux_registry_t *reg) {
    memset(srv, 0, sizeof(*srv));
    srv->registry   = reg;
    srv->running    = 1;
    srv->listen_fd  = -1;
    srv->epoll_fd   = -1;

    /* Initialise client fd side-table */
    for (int i = 0; i < 4096; i++) g_client_fd_by_dl[i] = -1;
    for (int i = 0; i < FLUX_IPC_MAX_CLIENTS; i++) srv->clients[i].fd = -1;

    /* Create Unix socket */
    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (srv->listen_fd < 0) {
        flux_set_syserror("socket");
        return -1;
    }

    unlink(FLUX_IPC_SOCK_PATH);   /* remove stale socket */

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FLUX_IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        flux_set_error(FLUX_ERR_IPC_BIND, "bind(%s): %s",
                       FLUX_IPC_SOCK_PATH, strerror(errno));
        goto cleanup;
    }

    if (listen(srv->listen_fd, FLUX_IPC_BACKLOG) < 0) {
        flux_set_syserror("listen");
        goto cleanup;
    }

    /* epoll */
    srv->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->epoll_fd < 0) { flux_set_syserror("epoll_create1"); goto cleanup; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev);

    fprintf(stderr, "[fluxdm-core] IPC listening on %s\n", FLUX_IPC_SOCK_PATH);

    /* Event loop */
    struct epoll_event events[FLUX_IPC_MAX_CLIENTS + 4];
    while (srv->running) {
        int nfds = epoll_wait(srv->epoll_fd, events,
                              FLUX_IPC_MAX_CLIENTS + 4, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            flux_set_syserror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == srv->listen_fd) {
                /* Accept new client */
                int cfd = accept4(srv->listen_fd, NULL, NULL,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) continue;

                /* Find a free slot */
                int slot = -1;
                for (int s = 0; s < FLUX_IPC_MAX_CLIENTS; s++) {
                    if (!srv->clients[s].active) { slot = s; break; }
                }
                if (slot < 0) { close(cfd); continue; }

                srv->clients[slot].fd       = cfd;
                srv->clients[slot].active   = 1;
                srv->clients[slot].recv_len = 0;

                struct epoll_event cev = {
                    .events   = EPOLLIN | EPOLLET,
                    .data.ptr = &srv->clients[slot],
                };
                epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, cfd, &cev);
                fprintf(stderr, "[fluxdm-core] client connected (slot %d)\n", slot);
            } else {
                /* Readable client */
                flux_ipc_client_t *c = events[i].data.ptr;
                if (c && c->active) client_readable(srv, c);
            }
        }
    }

cleanup:
    for (int s = 0; s < FLUX_IPC_MAX_CLIENTS; s++) {
        if (srv->clients[s].fd >= 0) close(srv->clients[s].fd);
    }
    if (srv->listen_fd >= 0) { close(srv->listen_fd); unlink(FLUX_IPC_SOCK_PATH); }
    if (srv->epoll_fd  >= 0)   close(srv->epoll_fd);
    return 0;
}

void flux_ipc_stop(flux_ipc_server_t *srv) {
    srv->running = 0;
}
