/* FluxDM — ipc/messages.h
 * Payload layouts for every flux_msg_type_t.
 * Sent immediately after flux_header_t, length = payload_len.
 */
#pragma once

#include "protocol.h"
#include <stdint.h>

/* ── CMD_START ────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint16_t segments;
    uint16_t flags;
    uint64_t bandwidth_limit;
    uint8_t  checksum_type;
    uint8_t  checksum[32];
    uint8_t  _pad[7];
    /* followed by: uint16_t url_len; char url[url_len];
     *              uint16_t path_len; char path[path_len]; */
} flux_cmd_start_t;

/* ── CMD_SET_LIMIT ────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t bytes_per_sec;
} flux_cmd_set_limit_t;

/* ── RSP_ERROR ────────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  code;          /* see FLUX_WIRE_ERR_* in protocol.h */
    uint8_t  _pad[3];
    char     message[124];
} flux_rsp_error_t;

/* ── RSP_PROGRESS ─────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint64_t total_bytes;
    uint64_t downloaded_bytes;
    uint32_t speed_bps;
    uint32_t eta_seconds;
    uint8_t  active_segments;
    uint8_t  _pad[3];
} flux_rsp_progress_t;

/* ── RSP_COMPLETE ─────────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint64_t total_bytes;
    uint32_t elapsed_ms;
    uint8_t  checksum_ok;
    uint8_t  checksum[32];
    uint8_t  _pad[3];
} flux_rsp_complete_t;

/* ── RSP_STATUS ───────────────────────────────────────────────────────────── */
typedef enum __attribute__((packed)) {
    FLUX_STATUS_IDLE        = 0,
    FLUX_STATUS_CONNECTING  = 1,
    FLUX_STATUS_DOWNLOADING = 2,
    FLUX_STATUS_PAUSED      = 3,
    FLUX_STATUS_COMPLETE    = 4,
    FLUX_STATUS_ERROR       = 5,
    FLUX_STATUS_CANCELLED   = 6,
} flux_download_status_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t  status;
    uint8_t  active_segments;
    uint8_t  total_segments;
    uint8_t  _pad;
    uint64_t total_bytes;
    uint64_t downloaded_bytes;
    uint32_t speed_bps;
    uint32_t eta_seconds;
} flux_rsp_status_t;
