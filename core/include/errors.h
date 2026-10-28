/* FluxDM — core/include/errors.h
 * Thread-safe error handling. Every public function that can fail
 * sets a thread-local error via flux_set_error() and returns -1 / NULL.
 */
#pragma once

#include <stddef.h>

/* ── Error codes ──────────────────────────────────────────────────────────── */
typedef enum {
    FLUX_OK              =  0,
    FLUX_ERR_NONE_       =  0,   /* alias */

    /* System / I/O */
    FLUX_ERR_SYS         =  1,   /* check errno */
    FLUX_ERR_NOMEM       =  2,
    FLUX_ERR_TIMEOUT_    =  3,
    FLUX_ERR_WOULDBLOCK  =  4,

    /* Network */
    FLUX_ERR_CONNECT     = 10,
    FLUX_ERR_DNS         = 11,
    FLUX_ERR_BADADDR     = 12,
    FLUX_ERR_RESET       = 13,
    FLUX_ERR_PIPE        = 14,
    FLUX_ERR_NETWORK     = 15,   /* generic network error */
    FLUX_ERR_BAD_URL     = 16,   /* URL could not be parsed / unsupported scheme */

    /* TLS */
    FLUX_ERR_TLS_INIT    = 20,
    FLUX_ERR_TLS_HNDSHK  = 21,
    FLUX_ERR_TLS_IO      = 22,
    FLUX_ERR_TLS_CERT    = 23,

    /* HTTP */
    FLUX_ERR_HTTP_PARSE  = 30,
    FLUX_ERR_HTTP_STATUS = 31,   /* unexpected status code   */
    FLUX_ERR_HTTP_RANGE  = 32,   /* server rejected range    */
    FLUX_ERR_HTTP_REDIR  = 33,   /* too many redirects       */
    FLUX_ERR_FORBIDDEN   = 34,   /* HTTP 403 */
    FLUX_ERR_SERVER      = 35,   /* HTTP 5xx */
    FLUX_ERR_REMOTE_CHANGED = 36, /* file changed on the server mid-download */

    /* File / Disk */
    FLUX_ERR_DISK_OPEN   = 40,
    FLUX_ERR_DISK_WRITE  = 41,
    FLUX_ERR_DISK_READ   = 42,
    FLUX_ERR_DISK_SEEK   = 43,
    FLUX_ERR_DISK_SPACE  = 44,

    /* Download state */
    FLUX_ERR_BAD_STATE   = 50,
    FLUX_ERR_DUPLICATE   = 51,
    FLUX_ERR_NOT_FOUND_  = 52,
    FLUX_ERR_CHECKSUM_   = 53,

    /* IPC */
    FLUX_ERR_IPC_BIND    = 60,
    FLUX_ERR_IPC_ACCEPT  = 61,
    FLUX_ERR_IPC_MSG     = 62,
    FLUX_ERR_IPC_PROTO   = 63,

    FLUX_ERR_INTERNAL    = 98,
    FLUX_ERR_UNKNOWN     = 99,
} flux_error_t;

/* ── Thread-local error store ─────────────────────────────────────────────── */

/* Set the error for the calling thread. fmt is a printf-style detail string.
 * Stores both the code and a formatted message (truncated to 255 chars). */
void flux_set_error(flux_error_t code, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Retrieve the last error code for the calling thread. */
flux_error_t flux_last_error(void);

/* Retrieve a human-readable description of the last error.
 * The returned pointer is valid until the next flux_set_error() call
 * on this thread. Never NULL. */
const char *flux_last_error_str(void);

/* Convenience: reset the thread-local error to FLUX_OK. */
void flux_clear_error(void);

/* Map a flux_error_t to a short static string (no detail). */
const char *flux_strerror(flux_error_t code);

/* Propagate errno into flux error store. */
void flux_set_syserror(const char *context);

/* ── Logging ──────────────────────────────────────────────────────────────── */
/* Process-wide log verbosity: 0 = silent, 1 = info, 2 = debug.
 * flux_log() formats into one buffer and emits it with a single write(2) so
 * lines from different download threads never interleave mid-line. */
void flux_log_set_level(int level);
int  flux_log_level(void);
void flux_log(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
