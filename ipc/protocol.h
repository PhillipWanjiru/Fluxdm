/* FluxDM — ipc/protocol.h
 * Wire protocol between the C core and the Java daemon.
 * All multi-byte integers are little-endian.
 */
#pragma once

#include <stdint.h>

/* ── Magic / version ──────────────────────────────────────────────────────── */
#define FLUX_MAGIC        0x464C5558u   /* "FLUX" */
#define FLUX_PROTO_VER    1u

/* ── Maximum sizes (guarded so including both protocol.h and http.h is safe) */
#ifndef FLUX_MAX_URL
#  define FLUX_MAX_URL     4096
#endif
#ifndef FLUX_MAX_PATH
#  define FLUX_MAX_PATH    1024
#endif
#ifndef FLUX_MAX_PAYLOAD
#  define FLUX_MAX_PAYLOAD (1u << 20)   /* 1 MiB hard cap per message */
#endif

/* ── Message types ────────────────────────────────────────────────────────── */
typedef enum __attribute__((packed)) {
    /* Commands  (Java → C) */
    FLUX_CMD_START        = 0x01,
    FLUX_CMD_PAUSE        = 0x02,
    FLUX_CMD_RESUME       = 0x03,
    FLUX_CMD_CANCEL       = 0x04,
    FLUX_CMD_SET_LIMIT    = 0x05,
    FLUX_CMD_STATUS       = 0x06,
    FLUX_CMD_STATUS_ALL   = 0x07,
    FLUX_CMD_SHUTDOWN     = 0xFF,

    /* Responses (C → Java) */
    FLUX_RSP_OK           = 0x10,
    FLUX_RSP_ERROR        = 0x11,
    FLUX_RSP_PROGRESS     = 0x12,
    FLUX_RSP_COMPLETE     = 0x13,
    FLUX_RSP_STATUS       = 0x14,
} flux_msg_type_t;

/* ── Frame header (fixed 16 bytes) ────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;          /* flux_msg_type_t               */
    uint16_t flags;         /* reserved, set to 0            */
    uint32_t id;            /* download id (0 = global)      */
    uint32_t payload_len;   /* bytes that follow this header */
} flux_header_t;

#ifdef __cplusplus
static_assert(sizeof(flux_header_t) == 16, "flux_header_t must be 16 bytes");
#else
_Static_assert(sizeof(flux_header_t) == 16, "flux_header_t must be 16 bytes");
#endif

/* ── Wire error codes (integer values — matches flux_error_t in errors.h) ─── */
#define FLUX_WIRE_ERR_NONE         0
#define FLUX_WIRE_ERR_INVALID_URL  1
#define FLUX_WIRE_ERR_NETWORK      2
#define FLUX_WIRE_ERR_TIMEOUT      3
#define FLUX_WIRE_ERR_DISK         4
#define FLUX_WIRE_ERR_TLS          5
#define FLUX_WIRE_ERR_NOT_FOUND    6
#define FLUX_WIRE_ERR_FORBIDDEN    7
#define FLUX_WIRE_ERR_SERVER       8
#define FLUX_WIRE_ERR_RANGE        9
#define FLUX_WIRE_ERR_CHECKSUM     10
#define FLUX_WIRE_ERR_INTERNAL     99
