/* FluxDM — core/include/protocol/https.h
 * OpenSSL TLS 1.2/1.3 layer that wraps flux_conn_t.
 * Called by connection.c during flux_conn_finish_connect().
 */
#pragma once

#include "../connection.h"
#include <openssl/ssl.h>

/* Shared OpenSSL context — initialised once at startup */
typedef struct {
    SSL_CTX *ctx;
} flux_ssl_ctx_t;

/* One-time OpenSSL initialisation (call from main before spawning threads). */
int  flux_tls_global_init(void);
void flux_tls_global_cleanup(void);

/* Attach a TLS session to an already-connected fd and drive the first
 * handshake step. A real TLS handshake often needs several round trips,
 * so this frequently does NOT complete in one call — the caller must
 * check the return value and re-arm epoll accordingly rather than
 * assuming the connection is ready.
 * Returns: 1 = handshake complete, conn->state should become READY
 *          0 = in progress, needs EPOLLIN to continue
 *          2 = in progress, needs EPOLLOUT to continue
 *         -1 = error (flux_set_error already called) */
int  flux_tls_attach(flux_conn_t *c);

/* Expose the shared global SSL_CTX for callers that need a raw, blocking
 * TLS session outside the segment/epoll machinery — specifically the
 * synchronous HEAD probe in downloader.c, which uses a plain blocking
 * socket and needs SSL_connect()/SSL_write()/SSL_read() directly rather
 * than the non-blocking step-driven API above. Returns NULL if
 * flux_tls_global_init() hasn't been called yet. */
SSL_CTX *flux_tls_get_global_ctx(void);

/* Drive one more step of a handshake already started by flux_tls_attach().
 * Call this when epoll fires on a connection sitting in SEG_TLS_SHAKE.
 * Returns the same 1 / 0 / 2 / -1 contract as flux_tls_attach() above. */
int  flux_tls_handshake_step(flux_conn_t *c);

/* Detach and free TLS resources without closing the fd. */
void flux_tls_detach(flux_conn_t *c);

/* TLS-aware send/recv (called by connection.c when c->use_tls is set). */
ssize_t flux_tls_read (flux_conn_t *c, void *buf,       size_t len);
ssize_t flux_tls_write(flux_conn_t *c, const void *buf, size_t len);
