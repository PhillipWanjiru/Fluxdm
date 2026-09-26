/* FluxDM — core/include/downloader.h
 * The central download object that owns segments, the epoll loop,
 * the resume state, checksum context, and bandwidth bucket.
 */
#pragma once

#include "segment.h"
#include "resume.h"
#include "bandwidth.h"
#include "checksum.h"
#include "connection.h"
#include "protocol/http.h"
#include "../../ipc/messages.h"
#include <stdint.h>
#include <pthread.h>

#define FLUX_DL_MAX_SEGMENTS  FLUX_MAX_SEGMENTS
#define FLUX_EPOLL_MAX_EVENTS 128
#define FLUX_PROGRESS_INTERVAL_MS 250   /* push progress every 250 ms */

/* ── Download state ───────────────────────────────────────────────────────── */
typedef enum {
    DL_IDLE        = 0,
    DL_PROBING     = 1,   /* HEAD request to get file size + capabilities */
    DL_RUNNING     = 2,
    DL_PAUSED      = 3,
    DL_COMPLETE    = 4,
    DL_ERROR       = 5,
    DL_CANCELLED   = 6,
} dl_state_t;

/* ── Download object ──────────────────────────────────────────────────────── */
typedef struct flux_download {
    uint32_t        id;
    dl_state_t      state;

    /* URL components */
    char            url[FLUX_MAX_URL];
    char            host[256];
    char            url_path[FLUX_MAX_URL];
    uint16_t        port;
    uint8_t         use_tls;

    /* Output file */
    char            file_path[FLUX_MAX_PATH];
    char            part_path[FLUX_MAX_PATH + 16];
    int             file_fd;

    /* File metadata (from HEAD) */
    uint64_t        total_size;     /* 0 if server doesn't report it    */
    uint8_t         accept_ranges;

    /* Segments */
    flux_segment_t *segs[FLUX_DL_MAX_SEGMENTS];
    uint8_t         num_segs;
    uint8_t         num_segs_active;

    /* Progress (updated atomically from segment callbacks) */
    uint64_t        downloaded_bytes;   /* __atomic operations          */
    uint64_t        last_progress_ms;
    uint32_t        speed_bps;

    /* Bandwidth */
    flux_bucket_t   bw_bucket;         /* per-download limit            */
    flux_bucket_t  *global_bucket;     /* pointer to daemon global      */

    /* Checksum */
    flux_cksum_ctx_t *cksum;
    uint8_t          cksum_type;
    uint8_t          cksum_expected[FLUX_CKSUM_MAX_LEN];

    /* Resume */
    flux_resume_t   resume;

    /* epoll */
    int             epoll_fd;
    int             timer_fd;           /* timerfd for retry/throttle wakeup */

    /* HTTP: per-segment response buffers now live in flux_segment_t */

    /* Connection pool (reused across segments for keep-alive) */
    flux_conn_pool_t conn_pool;

    /* IPC callback — called when progress/complete events occur */
    void (*on_event)(struct flux_download *dl, flux_msg_type_t event);

    /* Thread that runs this download's epoll loop */
    pthread_t        thread;
    pthread_mutex_t  lock;
    int              pipe_cancel[2];    /* write to [1] to abort        */
} flux_download_t;

/* ── Downloader registry ──────────────────────────────────────────────────── */
typedef struct {
    flux_download_t *downloads[4096];
    int              count;
    flux_bucket_t    global_bw;
    pthread_mutex_t  lock;
} flux_registry_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* One-time init of the global registry. Call from main(). */
int  flux_registry_init(flux_registry_t *reg);
void flux_registry_destroy(flux_registry_t *reg);

/* Create and start a new download. Spawns an internal epoll thread.
 * Returns the download on success, NULL on error.  */
flux_download_t *flux_dl_start(flux_registry_t   *reg,
                               uint32_t           id,
                               const char        *url,
                               const char        *dest_path,
                               uint8_t            num_segs,
                               uint64_t           bw_limit,
                               flux_cksum_type_t  cksum_type,
                               const uint8_t     *cksum_expected,
                               void (*on_event)(flux_download_t *, flux_msg_type_t));

/* Pause a running download. Waits for all segments to flush. */
int  flux_dl_pause(flux_download_t *dl);

/* Resume a paused download. */
int  flux_dl_resume(flux_download_t *dl);

/* Cancel and remove the download. Leaves the partial file on disk. */
int  flux_dl_cancel(flux_download_t *dl);

/* Look up by id. Returns NULL if not found. */
flux_download_t *flux_registry_find(flux_registry_t *reg, uint32_t id);

/* ── Internal callbacks (called from segment.c) ───────────────────────────── */
void flux_dl_on_progress(flux_download_t *dl, uint64_t bytes_written);
int  flux_dl_bw_consume(flux_download_t *dl, size_t bytes);

/* ── Internal: epoll thread entry ─────────────────────────────────────────── */
void *flux_dl_thread(void *arg);
