/* FluxDM — core/include/connection.h
 * Non-blocking TCP connection with optional TLS.
 * Connections are keyed by host:port and reused across segments
 * where the server supports HTTP/1.1 keep-alive.
 */
#pragma once

#include "errors.h"
#include <stdint.h>
#include <stddef.h>
#include <netdb.h>          /* addrinfo */

/* ── Tunables ─────────────────────────────────────────────────────────────── */
#define FLUX_CONN_POOL_MAX      64      /* max cached idle connections        */
#define FLUX_CONN_CONNECT_TOMS  10000   /* connect timeout milliseconds       */
#define FLUX_CONN_RECV_TOMS     30000   /* recv timeout milliseconds          */
#define FLUX_CONN_SEND_TOMS     15000   /* send timeout milliseconds          */
#define FLUX_CONN_SNDBUF        (128 * 1024)
#define FLUX_CONN_RCVBUF        (512 * 1024)

/* ── Connection state ─────────────────────────────────────────────────────── */
typedef enum {
    CONN_STATE_IDLE        = 0,
    CONN_STATE_RESOLVING   = 1,
    CONN_STATE_CONNECTING  = 2,
    CONN_STATE_READY       = 3,
    CONN_STATE_CLOSED      = 4,
    CONN_STATE_ERROR       = 5,
} conn_state_t;

/* ── Opaque TLS context ───────────────────────────────────────────────────── */
typedef struct flux_tls flux_tls_t;

/* ── Connection object ────────────────────────────────────────────────────── */
typedef struct flux_conn {
    int          fd;
    conn_state_t state;
    uint8_t      use_tls;
    flux_tls_t  *tls;               /* NULL for plain HTTP              */

    char         host[256];
    uint16_t     port;

    /* Optional pre-resolved peer address. When addr_len > 0 the connect
     * goes straight to it and skips DNS entirely — the probe resolves the
     * host once and every segment reuses the address that actually worked
     * (which also avoids retrying a dead IPv6 address on every segment). */
    struct sockaddr_storage addr;
    socklen_t    addr_len;

    /* Read buffer: bytes [rbuf_start, rbuf_end) are unread. Linear, compacted by
     * flux_conn_fill() when it runs out of room at the end. */
    uint8_t     *rbuf;
    size_t       rbuf_cap;
    size_t       rbuf_start;        /* read cursor                      */
    size_t       rbuf_end;          /* write cursor                     */

    uint64_t     bytes_sent;
    uint64_t     bytes_recv;

    int          keepalive;         /* 1 if server supports keep-alive  */
    struct flux_conn *pool_next;    /* intrusive pool free-list         */
} flux_conn_t;

/* ── Pool ─────────────────────────────────────────────────────────────────── */
typedef struct {
    flux_conn_t *head;              /* free-list of idle connections    */
    int          count;
} flux_conn_pool_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Allocate a connection struct (does NOT connect yet). */
flux_conn_t *flux_conn_new(const char *host, uint16_t port, int use_tls);

/* Pin the peer address (skips getaddrinfo in flux_conn_start_connect). */
void flux_conn_set_addr(flux_conn_t *c, const struct sockaddr *sa, socklen_t len);

/* Initiate a non-blocking connect. Call flux_conn_finish_connect()
 * once epoll signals EPOLLOUT on the fd. Returns 0 on success. */
int flux_conn_start_connect(flux_conn_t *c);

/* Complete a non-blocking connect after EPOLLOUT fires. For https://
 * connections this also drives the first TLS handshake step, which
 * frequently does NOT finish in one call — check the return value:
 * 1 = ready, 0 = needs EPOLLIN, 2 = needs EPOLLOUT, -1 = error.
 * On 0 or 2, call flux_conn_tls_handshake_continue() on the next epoll
 * event for whichever direction was requested. */
int flux_conn_finish_connect(flux_conn_t *c);

/* Drives one more step of a TLS handshake that flux_conn_finish_connect()
 * left in progress. Same 1 / 0 / 2 / -1 contract. */
int flux_conn_tls_handshake_continue(flux_conn_t *c);

/* Close the connection and free all resources (including TLS). */
void flux_conn_close(flux_conn_t *c);

/* ── I/O ──────────────────────────────────────────────────────────────────── */

/* Write exactly len bytes. Returns bytes written or -1 on error.
 * May return FLUX_ERR_WOULDBLOCK — caller should wait for EPOLLOUT. */
ssize_t flux_conn_send(flux_conn_t *c, const void *buf, size_t len);

/* Read up to len bytes into buf. Returns bytes read or -1 on error.
 * Returns 0 on clean EOF. May return FLUX_ERR_WOULDBLOCK. */
ssize_t flux_conn_recv(flux_conn_t *c, void *buf, size_t len);

/* Fill the internal read buffer from the socket (call on EPOLLIN).
 * Returns bytes buffered, 0 on EOF, -1 on error. */
ssize_t flux_conn_fill(flux_conn_t *c);

/* Read from the buffer without touching the socket.
 * Returns bytes consumed. */
size_t flux_conn_drain(flux_conn_t *c, void *buf, size_t len);

/* Peek at buffered data without consuming it. */
size_t flux_conn_peek(const flux_conn_t *c, void *buf, size_t len);

/* Bytes currently buffered and ready to read. */
size_t flux_conn_buffered(const flux_conn_t *c);

/* ── Pool helpers ─────────────────────────────────────────────────────────── */
void         flux_pool_init(flux_conn_pool_t *pool);
flux_conn_t *flux_pool_acquire(flux_conn_pool_t *pool,
                               const char *host, uint16_t port, int tls);
void         flux_pool_release(flux_conn_pool_t *pool, flux_conn_t *c);
void         flux_pool_purge(flux_conn_pool_t *pool);
