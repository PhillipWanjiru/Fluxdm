/* FluxDM — core/src/ipc/socket_server.h
 * Unix-domain-socket server for the Java daemon (and any other client).
 *
 * Threads and ordering — the rules everything here follows:
 *
 *  - ONE thread (the caller of flux_ipc_server_run) reads commands, owns the
 *    download registry and replies to commands.
 *  - Download workers push events (progress, completion, asynchronous errors)
 *    from their own threads. Events carry FLUX_FLAG_EVENT; replies never do, so
 *    a client can always tell "the answer to my PAUSE" from "download 7 just
 *    failed".
 *  - Every frame goes through a per-client output queue and is appended whole
 *    under that client's lock, so frames from different threads can never
 *    interleave mid-frame. Nothing here ever blocks on a slow client: a full
 *    socket buffer leaves the rest queued until EPOLLOUT; progress frames are
 *    dropped (the next one supersedes them) when a client falls behind, and a
 *    client that falls hopelessly behind is disconnected.
 *  - Events are broadcast to every connected client. There is no per-download
 *    routing table to go stale when a client reconnects.
 *  - A START is acknowledged BEFORE its download is launched, so the client
 *    always sees OK first and the events after.
 */
#pragma once

#include "../../include/downloader.h"
#include "../../../ipc/protocol.h"
#include "../../../ipc/messages.h"
#include "frame_io.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* ── Configuration ────────────────────────────────────────────────────────── */
#define FLUX_IPC_BACKLOG        8
#define FLUX_IPC_MAX_CLIENTS    16
#define FLUX_IPC_OUT_SOFT       (256u * 1024u)        /* queued bytes above which progress frames are dropped */
#define FLUX_IPC_OUT_HARD       (8u * 1024u * 1024u)  /* ... above which the client is disconnected          */
#define FLUX_IPC_REAP_MS        1000u

struct flux_ipc_server;

/* ── One connected client ─────────────────────────────────────────────────── */
typedef struct {
    struct flux_ipc_server *srv;
    int                     fd;          /* -1 = free slot                                  */
    int                     closing;     /* __atomic: any thread may set; the IPC thread closes */
    uint32_t                serial;      /* connection number, for logs                     */

    flux_frame_reader_t     rd;          /* IPC thread only                                 */

    pthread_mutex_t         out_lock;    /* guards everything below                         */
    uint8_t                *out;         /* pending output: bytes [out_off, out_len)        */
    size_t                  out_cap, out_len, out_off;
    int                     out_armed;   /* EPOLLOUT currently registered                   */
} flux_ipc_client_t;

/* ── The server ───────────────────────────────────────────────────────────── */
typedef struct flux_ipc_server {
    int                 listen_fd;
    int                 epoll_fd;
    int                 wake_fd;         /* eventfd: flux_ipc_stop() → the loop             */
    int                 running;         /* __atomic                                        */
    int                 owns_socket;     /* we bound sock_path and must unlink it           */
    char                sock_path[108];  /* sizeof(sockaddr_un.sun_path)                    */
    flux_registry_t    *registry;
    uint32_t            next_serial;

    pthread_mutex_t     clients_lock;    /* slot lifecycle vs. broadcasts from workers      */
    flux_ipc_client_t   clients[FLUX_IPC_MAX_CLIENTS];
} flux_ipc_server_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Create the listening socket at `sock_path` (NULL = the private default from
 * sock_path.h, mode 0600). Refuses to take over a socket that a live process is
 * serving; a stale socket file from a crashed run is replaced. 0 on success,
 * -1 (error set). */
int  flux_ipc_server_init(flux_ipc_server_t *srv, flux_registry_t *reg, const char *sock_path);

/* Serve clients until flux_ipc_stop() or CMD_SHUTDOWN. 0 on a clean stop. */
int  flux_ipc_server_run(flux_ipc_server_t *srv);

/* Ask the loop to exit. Async-signal-safe: callable from a signal handler. */
void flux_ipc_stop(flux_ipc_server_t *srv);

/* Close every client and the listening socket, unlink the socket file. */
void flux_ipc_server_close(flux_ipc_server_t *srv);

/* ── Events ───────────────────────────────────────────────────────────────── */

/* Registered with every download (flux_download_t::on_event). Called from the
 * worker thread; broadcasts the matching frame to all clients. */
void flux_ipc_on_dl_event(flux_download_t *dl, flux_msg_type_t event);

/* Map an internal error to a FLUX_WIRE_ERR_* value. */
uint8_t flux_ipc_wire_error(flux_error_t code);
