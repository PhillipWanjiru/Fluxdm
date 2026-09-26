/* FluxDM — core/src/protocol/https.c */

#include "../../include/protocol/https.h"
#include "../../include/errors.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* ── Module-level TLS context ─────────────────────────────────────────────── */
static SSL_CTX *g_ssl_ctx = NULL;

struct flux_tls {
    SSL *ssl;
};

/* ── Global init ──────────────────────────────────────────────────────────── */
int flux_tls_global_init(void) {
    if (g_ssl_ctx) return 0;   /* already initialised */

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    g_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ssl_ctx) {
        flux_set_error(FLUX_ERR_TLS_INIT, "SSL_CTX_new failed");
        return -1;
    }

    /* Require TLS 1.2 minimum */
    SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_2_VERSION);

    /* Enable certificate verification against the system store */
    SSL_CTX_set_default_verify_paths(g_ssl_ctx);
    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER, NULL);

    /* Prefer server-side cipher order, enable session reuse */
    SSL_CTX_set_options(g_ssl_ctx,
        SSL_OP_NO_SSLv2  | SSL_OP_NO_SSLv3 |
        SSL_OP_NO_TLSv1  | SSL_OP_NO_TLSv1_1 |
        SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* Enable session caching to speed up keep-alive reconnects */
    SSL_CTX_set_session_cache_mode(g_ssl_ctx, SSL_SESS_CACHE_CLIENT);

    return 0;
}

void flux_tls_global_cleanup(void) {
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = NULL; }
    EVP_cleanup();
    ERR_free_strings();
}

SSL_CTX *flux_tls_get_global_ctx(void) {
    return g_ssl_ctx;
}

/* ── Attach TLS to a connected fd ─────────────────────────────────────────── */
/* Returns the same tri-state result as flux_tls_handshake_step():
 * 1 = complete, 0 = needs EPOLLIN, 2 = needs EPOLLOUT, -1 = error. */
int flux_tls_attach(flux_conn_t *c) {
    if (!g_ssl_ctx) {
        flux_set_error(FLUX_ERR_TLS_INIT, "TLS not initialised");
        return -1;
    }

    flux_tls_t *t = calloc(1, sizeof(*t));
    if (!t) {
        flux_set_error(FLUX_ERR_NOMEM, "flux_tls_attach: calloc");
        return -1;
    }

    t->ssl = SSL_new(g_ssl_ctx);
    if (!t->ssl) {
        free(t);
        flux_set_error(FLUX_ERR_TLS_INIT, "SSL_new failed");
        return -1;
    }

    /* SNI: required for virtual hosting */
    SSL_set_tlsext_host_name(t->ssl, c->host);

    /* Hostname verification */
    SSL_set_hostflags(t->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set1_host(t->ssl, c->host);

    SSL_set_fd(t->ssl, c->fd);
    SSL_set_connect_state(t->ssl);
    c->tls = t;

    /* Drive the first handshake step and return its real result —
     * the caller (flux_conn_finish_connect) decides what to do with
     * "still in progress" rather than this function papering over it. */
    return flux_tls_handshake_step(c);
}

/* ── Handshake driver (non-blocking) ──────────────────────────────────────── */
/* Returns: 1 = handshake complete
 *          0 = in progress, needs EPOLLIN (SSL_ERROR_WANT_READ)
 *          2 = in progress, needs EPOLLOUT (SSL_ERROR_WANT_WRITE)
 *         -1 = hard error (flux_set_error already called)
 *
 * Distinguishing WANT_READ from WANT_WRITE matters: a real TLS
 * handshake typically needs several round trips, and each step can
 * want either direction (TLS 1.3 in particular can flip mid-handshake).
 * Arming epoll for the wrong direction after a WANT_WRITE — which the
 * previous version of this function didn't distinguish at all — means
 * the segment never gets woken up again and the download hangs forever
 * with no error and no progress. */
int flux_tls_handshake_step(flux_conn_t *c) {
    if (!c->tls || !c->tls->ssl) return -1;

    int rc = SSL_do_handshake(c->tls->ssl);
    if (rc == 1) return 1;   /* done */

    int err = SSL_get_error(c->tls->ssl, rc);
    if (err == SSL_ERROR_WANT_READ)  return 0;
    if (err == SSL_ERROR_WANT_WRITE) return 2;

    /* Real error */
    char errbuf[256];
    ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
    flux_set_error(FLUX_ERR_TLS_HNDSHK, "TLS handshake failed: %s", errbuf);
    return -1;
}

void flux_tls_detach(flux_conn_t *c) {
    if (!c->tls) return;
    if (c->tls->ssl) {
        SSL_shutdown(c->tls->ssl);
        SSL_free(c->tls->ssl);
    }
    free(c->tls);
    c->tls = NULL;
}

/* ── I/O ──────────────────────────────────────────────────────────────────── */
ssize_t flux_tls_read(flux_conn_t *c, void *buf, size_t len) {
    if (!c->tls || !c->tls->ssl) {
        flux_set_error(FLUX_ERR_TLS_IO, "tls_read: no TLS context");
        return -1;
    }

    int n = SSL_read(c->tls->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;

    int err = SSL_get_error(c->tls->ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;
        flux_set_error(FLUX_ERR_WOULDBLOCK, "SSL_read WANT");
        return -1;
    }
    if (err == SSL_ERROR_ZERO_RETURN) return 0;   /* clean EOF */

    char errbuf[256];
    ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
    flux_set_error(FLUX_ERR_TLS_IO, "SSL_read: %s", errbuf);
    return -1;
}

ssize_t flux_tls_write(flux_conn_t *c, const void *buf, size_t len) {
    if (!c->tls || !c->tls->ssl) {
        flux_set_error(FLUX_ERR_TLS_IO, "tls_write: no TLS context");
        return -1;
    }

    int n = SSL_write(c->tls->ssl, buf, (int)len);
    if (n > 0) return (ssize_t)n;

    int err = SSL_get_error(c->tls->ssl, n);
    if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
        errno = EAGAIN;
        flux_set_error(FLUX_ERR_WOULDBLOCK, "SSL_write WANT");
        return -1;
    }

    char errbuf[256];
    ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
    flux_set_error(FLUX_ERR_TLS_IO, "SSL_write: %s", errbuf);
    return -1;
}
