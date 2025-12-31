/* FluxDM — core/src/errors.c */

#include "../include/errors.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Thread-local state ───────────────────────────────────────────────────── */
#define FLUX_ERR_MSG_LEN 256

typedef struct {
    flux_error_t code;
    char         msg[FLUX_ERR_MSG_LEN];
} flux_tls_err_t;

static _Thread_local flux_tls_err_t tls_err = { FLUX_OK, "OK" };

/* ── Public API ───────────────────────────────────────────────────────────── */

void flux_set_error(flux_error_t code, const char *fmt, ...) {
    tls_err.code = code;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(tls_err.msg, FLUX_ERR_MSG_LEN, fmt, ap);
        va_end(ap);
    } else {
        strncpy(tls_err.msg, flux_strerror(code), FLUX_ERR_MSG_LEN - 1);
        tls_err.msg[FLUX_ERR_MSG_LEN - 1] = '\0';
    }
}

flux_error_t flux_last_error(void) {
    return tls_err.code;
}

const char *flux_last_error_str(void) {
    return tls_err.msg;
}

void flux_clear_error(void) {
    tls_err.code = FLUX_OK;
    strncpy(tls_err.msg, "OK", FLUX_ERR_MSG_LEN - 1);
}

void flux_set_syserror(const char *context) {
    int saved = errno;
    flux_set_error(FLUX_ERR_SYS, "%s: %s (errno %d)",
                   context ? context : "syscall", strerror(saved), saved);
}

const char *flux_strerror(flux_error_t code) {
    switch (code) {
        case FLUX_OK:             return "OK";
        case FLUX_ERR_SYS:        return "System error";
        case FLUX_ERR_NOMEM:      return "Out of memory";
        case FLUX_ERR_TIMEOUT_:   return "Operation timed out";
        case FLUX_ERR_WOULDBLOCK: return "Operation would block";
        case FLUX_ERR_CONNECT:    return "Connection failed";
        case FLUX_ERR_DNS:        return "DNS resolution failed";
        case FLUX_ERR_BADADDR:    return "Invalid address";
        case FLUX_ERR_RESET:      return "Connection reset by peer";
        case FLUX_ERR_PIPE:       return "Broken pipe";
        case FLUX_ERR_NETWORK:    return "Network error";
        case FLUX_ERR_BAD_URL:    return "Invalid or unsupported URL";
        case FLUX_ERR_TLS_INIT:   return "TLS initialisation failed";
        case FLUX_ERR_TLS_HNDSHK: return "TLS handshake failed";
        case FLUX_ERR_TLS_IO:     return "TLS I/O error";
        case FLUX_ERR_TLS_CERT:   return "TLS certificate error";
        case FLUX_ERR_HTTP_PARSE: return "HTTP parse error";
        case FLUX_ERR_HTTP_STATUS:return "Unexpected HTTP status";
        case FLUX_ERR_HTTP_RANGE: return "Server rejected byte-range";
        case FLUX_ERR_HTTP_REDIR: return "Too many HTTP redirects";
        case FLUX_ERR_FORBIDDEN:  return "HTTP 403 Forbidden";
        case FLUX_ERR_SERVER:     return "HTTP server error";
        case FLUX_ERR_REMOTE_CHANGED: return "File changed on the server";
        case FLUX_ERR_DISK_OPEN:  return "Cannot open file";
        case FLUX_ERR_DISK_WRITE: return "Disk write failed";
        case FLUX_ERR_DISK_READ:  return "Disk read failed";
        case FLUX_ERR_DISK_SEEK:  return "Disk seek failed";
        case FLUX_ERR_DISK_SPACE: return "Insufficient disk space";
        case FLUX_ERR_BAD_STATE:  return "Invalid download state";
        case FLUX_ERR_DUPLICATE:  return "Duplicate download ID";
        case FLUX_ERR_NOT_FOUND_: return "Download not found";
        case FLUX_ERR_CHECKSUM_:  return "Checksum mismatch";
        case FLUX_ERR_IPC_BIND:   return "IPC bind failed";
        case FLUX_ERR_IPC_ACCEPT: return "IPC accept failed";
        case FLUX_ERR_IPC_MSG:    return "IPC message error";
        case FLUX_ERR_IPC_PROTO:  return "IPC protocol violation";
        case FLUX_ERR_INTERNAL:   return "Internal error";
        default:                  return "Unknown error";
    }
}

/* ── Logging ──────────────────────────────────────────────────────────────── */
static int g_log_level = 1;

void flux_log_set_level(int level) { g_log_level = level; }
int  flux_log_level(void)          { return g_log_level; }

void flux_log(int level, const char *fmt, ...) {
    if (level > g_log_level) return;

    char    line[512];
    int     n = snprintf(line, sizeof(line), "[fluxdm-core] ");
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) return;

    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;   /* truncated */
    line[len++] = '\n';

    ssize_t w = write(STDERR_FILENO, line, len);
    (void)w;
}
