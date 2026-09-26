/* FluxDM — core/src/ipc/socket_server.h
 * Unix domain socket server that accepts commands from the Java daemon
 * and dispatches them to the download registry.
 * One persistent connection per Java daemon session.
 * Protocol: flux_header_t + payload (see ipc/messages.h)
 */
#pragma once

#include "../../include/downloader.h"
#include "../../../ipc/protocol.h"
#include "../../../ipc/messages.h"
#include <stdint.h>
#include <stddef.h>

/* ── Configuration ────────────────────────────────────────────────────────── */
#define FLUX_IPC_SOCK_PATH      "/tmp/fluxdm.sock"
#define FLUX_IPC_BACKLOG        4
#define FLUX_IPC_RECV_BUF       (FLUX_MAX_PAYLOAD + sizeof(flux_header_t))
#define FLUX_IPC_MAX_CLIENTS    8

/* ── IPC client connection ────────────────────────────────────────────────── */
typedef struct {
    int      fd;
    uint8_t  recv_buf[FLUX_IPC_RECV_BUF];
    size_t   recv_len;          /* bytes currently buffered */
    uint8_t  active;
} flux_ipc_client_t;

/* ── IPC server ───────────────────────────────────────────────────────────── */
typedef struct {
    int                 listen_fd;
    flux_ipc_client_t   clients[FLUX_IPC_MAX_CLIENTS];
    int                 epoll_fd;
    flux_registry_t    *registry;
    volatile int        running;
} flux_ipc_server_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Bind to FLUX_IPC_SOCK_PATH and begin accepting clients.
 * Blocks until flux_ipc_stop() is called from another thread. */
int  flux_ipc_server_run(flux_ipc_server_t *srv, flux_registry_t *reg);

/* Signal the server loop to exit cleanly. Thread-safe. */
void flux_ipc_stop(flux_ipc_server_t *srv);

/* ── Message dispatch ─────────────────────────────────────────────────────── */

/* Called internally when a complete message has been received from a client.
 * Dispatches to the appropriate registry action and sends a response. */
int flux_ipc_dispatch(flux_ipc_server_t *srv, flux_ipc_client_t *client,
                      const flux_header_t *hdr, const uint8_t *payload);

/* ── Response helpers ─────────────────────────────────────────────────────── */

/* Send a simple RSP_OK frame (no payload). */
int flux_ipc_send_ok(int fd, uint32_t download_id);

/* Send an RSP_ERROR frame. */
int flux_ipc_send_error(int fd, uint32_t download_id,
                        uint8_t code, const char *msg);

/* Send an RSP_PROGRESS frame. */
int flux_ipc_send_progress(int fd, const flux_download_t *dl);

/* Send an RSP_COMPLETE frame. */
int flux_ipc_send_complete(int fd, const flux_download_t *dl);

/* ── Event callback registered with each download ────────────────────────── */
/* Signature matches flux_download_t::on_event */
void flux_ipc_on_dl_event(flux_download_t *dl, flux_msg_type_t event);
