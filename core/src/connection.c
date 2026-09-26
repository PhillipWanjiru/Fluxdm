/* FluxDM — core/src/connection.c */

#include "../include/connection.h"
#include "../include/errors.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>

#define RBUF_DEFAULT_CAP (512 * 1024)

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_socket_opts(int fd) {
    int yes = 1;
    /* TCP_NODELAY — disable Nagle; we send full HTTP requests at once */
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
        return -1;

    /* SO_KEEPALIVE — detect dead connections */
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) < 0)
        return -1;

    /* Tune send/recv buffers */
    int sndbuf = FLUX_CONN_SNDBUF;
    int rcvbuf = FLUX_CONN_RCVBUF;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

flux_conn_t *flux_conn_new(const char *host, uint16_t port, int use_tls) {
    flux_conn_t *c = calloc(1, sizeof(*c));
    if (!c) {
        flux_set_error(FLUX_ERR_NOMEM, "flux_conn_new: calloc failed");
        return NULL;
    }

    c->rbuf = malloc(RBUF_DEFAULT_CAP);
    if (!c->rbuf) {
        free(c);
        flux_set_error(FLUX_ERR_NOMEM, "flux_conn_new: rbuf malloc failed");
        return NULL;
    }

    c->rbuf_cap   = RBUF_DEFAULT_CAP;
    c->fd         = -1;
    c->state      = CONN_STATE_IDLE;
    c->use_tls    = (uint8_t)(use_tls != 0);
    c->port       = port;
    c->keepalive  = 0;
    c->pool_next  = NULL;

    strncpy(c->host, host, sizeof(c->host) - 1);
    return c;
}

int flux_conn_start_connect(flux_conn_t *c) {
    struct addrinfo hints = {0}, *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", c->port);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(c->host, port_str, &hints, &res);
    if (rc != 0) {
        flux_set_error(FLUX_ERR_DNS, "getaddrinfo(%s): %s",
                       c->host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        if (set_nonblocking(fd) < 0 || set_socket_opts(fd) < 0) {
            close(fd); fd = -1; continue;
        }

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            /* Instant connect (rare on non-loopback) */
            c->state = CONN_STATE_READY;
            break;
        }
        if (errno == EINPROGRESS) {
            /* Normal non-blocking path — wait for EPOLLOUT */
            c->state = CONN_STATE_CONNECTING;
            break;
        }
        close(fd); fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        flux_set_error(FLUX_ERR_CONNECT, "Cannot connect to %s:%u", c->host, c->port);
        return -1;
    }

    c->fd = fd;
    return 0;
}

/* Returns: 1 = fully ready (TCP connected, TLS done or not needed)
 *          0 = TLS handshake in progress, needs EPOLLIN to continue
 *          2 = TLS handshake in progress, needs EPOLLOUT to continue
 *         -1 = error
 *
 * A real TLS handshake against a real remote server almost always needs
 * multiple round trips. The previous version of this function called
 * flux_tls_attach() once and treated ANY non-negative result as "ready",
 * immediately proceeding to send the HTTP request over a connection
 * whose handshake hadn't actually finished — which left the segment
 * waiting on the wrong epoll direction forever with no error and no
 * progress. This was invisible throughout Phase 1 testing since every
 * test target was plain HTTP (no TLS at all), and only surfaced against
 * a real HTTPS server. */
int flux_conn_finish_connect(flux_conn_t *c) {
    if (c->state == CONN_STATE_READY) return 1;
    if (c->state != CONN_STATE_CONNECTING) {
        flux_set_error(FLUX_ERR_BAD_STATE, "finish_connect: not in CONNECTING state");
        return -1;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        flux_set_syserror("getsockopt SO_ERROR");
        c->state = CONN_STATE_ERROR;
        return -1;
    }
    if (err != 0) {
        errno = err;
        flux_set_syserror("connect");
        c->state = CONN_STATE_ERROR;
        return -1;
    }

    /* TLS handshake if needed — delegated to https.c */
    if (c->use_tls) {
        /* flux_tls_attach() is defined in protocol/https.c */
        extern int flux_tls_attach(flux_conn_t *c);
        int rc = flux_tls_attach(c);
        if (rc < 0) {
            c->state = CONN_STATE_ERROR;
            return -1;
        }
        if (rc == 1) {
            c->state = CONN_STATE_READY;
            return 1;
        }
        /* rc is 0 (want read) or 2 (want write): TCP is connected but
         * the TLS handshake needs more steps. The segment layer moves
         * to SEG_TLS_SHAKE and re-arms epoll for whichever direction rc
         * indicates, then calls flux_conn_tls_handshake_continue() on
         * the next wakeup. */
        return rc;
    }

    c->state = CONN_STATE_READY;
    return 1;
}

/* Drives one more step of a TLS handshake already started by a prior
 * flux_conn_finish_connect() call. Same 1/0/2/-1 contract. */
int flux_conn_tls_handshake_continue(flux_conn_t *c) {
    extern int flux_tls_handshake_step(flux_conn_t *c);
    int rc = flux_tls_handshake_step(c);
    if (rc < 0) {
        c->state = CONN_STATE_ERROR;
        return -1;
    }
    if (rc == 1) {
        c->state = CONN_STATE_READY;
        return 1;
    }
    return rc;   /* 0 = want read, 2 = want write */
}

void flux_conn_close(flux_conn_t *c) {
    if (!c) return;
    if (c->tls) {
        extern void flux_tls_detach(flux_conn_t *c);
        flux_tls_detach(c);
    }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    free(c->rbuf);
    free(c);
}

/* ── I/O ──────────────────────────────────────────────────────────────────── */

ssize_t flux_conn_send(flux_conn_t *c, const void *buf, size_t len) {
    if (c->state != CONN_STATE_READY) {
        flux_set_error(FLUX_ERR_BAD_STATE, "send on non-ready connection");
        return -1;
    }

    ssize_t sent = 0;
    const uint8_t *ptr = buf;

    while ((size_t)sent < len) {
        ssize_t n;
        if (c->use_tls && c->tls) {
            extern ssize_t flux_tls_write(flux_conn_t *c, const void *b, size_t l);
            n = flux_tls_write(c, ptr + sent, len - (size_t)sent);
        } else {
            n = send(c->fd, ptr + sent, len - (size_t)sent, MSG_NOSIGNAL);
        }

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                flux_set_error(FLUX_ERR_WOULDBLOCK, "send EAGAIN");
                return sent > 0 ? sent : -1;
            }
            flux_set_syserror("send");
            c->state = CONN_STATE_ERROR;
            return -1;
        }
        sent += n;
        c->bytes_sent += (uint64_t)n;
    }
    return sent;
}

ssize_t flux_conn_recv(flux_conn_t *c, void *buf, size_t len) {
    if (c->state != CONN_STATE_READY) {
        flux_set_error(FLUX_ERR_BAD_STATE, "recv on non-ready connection");
        return -1;
    }

    ssize_t n;
    if (c->use_tls && c->tls) {
        extern ssize_t flux_tls_read(flux_conn_t *c, void *b, size_t l);
        n = flux_tls_read(c, buf, len);
    } else {
        n = recv(c->fd, buf, len, 0);
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            flux_set_error(FLUX_ERR_WOULDBLOCK, "recv EAGAIN");
            return -1;
        }
        flux_set_syserror("recv");
        c->state = CONN_STATE_ERROR;
        return -1;
    }
    if (n == 0) { c->state = CONN_STATE_CLOSED; }
    c->bytes_recv += (uint64_t)n;
    return n;
}

/* Fill the ring buffer from the socket */
ssize_t flux_conn_fill(flux_conn_t *c) {
    /* Available space — ring buffer is linear (end never wraps in this impl) */
    size_t space = c->rbuf_cap - c->rbuf_end;
    if (space == 0) {
        /* Compact: slide data to start */
        size_t used = c->rbuf_end - c->rbuf_start;
        memmove(c->rbuf, c->rbuf + c->rbuf_start, used);
        c->rbuf_start = 0;
        c->rbuf_end   = used;
        space = c->rbuf_cap - c->rbuf_end;
        if (space == 0) {
            /* Buffer full — caller must drain before filling */
            flux_set_error(FLUX_ERR_WOULDBLOCK, "ring buffer full");
            return -1;
        }
    }

    ssize_t n = flux_conn_recv(c, c->rbuf + c->rbuf_end, space);
    if (n > 0) c->rbuf_end += (size_t)n;
    return n;
}

size_t flux_conn_drain(flux_conn_t *c, void *buf, size_t len) {
    size_t avail = c->rbuf_end - c->rbuf_start;
    size_t take  = avail < len ? avail : len;
    if (take > 0 && buf) memcpy(buf, c->rbuf + c->rbuf_start, take);
    c->rbuf_start += take;
    return take;
}

size_t flux_conn_peek(const flux_conn_t *c, void *buf, size_t len) {
    size_t avail = c->rbuf_end - c->rbuf_start;
    size_t take  = avail < len ? avail : len;
    if (take > 0 && buf) memcpy(buf, c->rbuf + c->rbuf_start, take);
    return take;
}

size_t flux_conn_buffered(const flux_conn_t *c) {
    return c->rbuf_end - c->rbuf_start;
}

/* ── Pool ─────────────────────────────────────────────────────────────────── */

void flux_pool_init(flux_conn_pool_t *pool) {
    pool->head  = NULL;
    pool->count = 0;
}

flux_conn_t *flux_pool_acquire(flux_conn_pool_t *pool,
                                const char *host, uint16_t port, int tls) {
    flux_conn_t **pp = &pool->head;
    while (*pp) {
        flux_conn_t *c = *pp;
        if (c->port == port && c->use_tls == (uint8_t)tls
                && strncmp(c->host, host, sizeof(c->host)) == 0
                && c->state == CONN_STATE_READY) {
            *pp = c->pool_next;
            c->pool_next = NULL;
            pool->count--;
            return c;
        }
        pp = &(*pp)->pool_next;
    }
    return NULL;   /* nothing suitable — caller must create new */
}

void flux_pool_release(flux_conn_pool_t *pool, flux_conn_t *c) {
    if (!c || c->state != CONN_STATE_READY || !c->keepalive) {
        flux_conn_close(c);
        return;
    }
    if (pool->count >= FLUX_CONN_POOL_MAX) {
        flux_conn_close(c);
        return;
    }
    c->rbuf_start = c->rbuf_end = 0;   /* reset buffer */
    c->pool_next  = pool->head;
    pool->head    = c;
    pool->count++;
}

void flux_pool_purge(flux_conn_pool_t *pool) {
    flux_conn_t *c = pool->head;
    while (c) {
        flux_conn_t *next = c->pool_next;
        flux_conn_close(c);
        c = next;
    }
    pool->head  = NULL;
    pool->count = 0;
}
