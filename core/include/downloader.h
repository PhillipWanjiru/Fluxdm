/* FluxDM — core/include/downloader.h
 * The central download object: owns the segments, the epoll loop, the resume
 * state and the bandwidth buckets.
 *
 * ── Threading model ─────────────────────────────────────────────────────────
 * Every download has exactly ONE worker thread. That thread alone touches the
 * segments, connections, file descriptor and resume state, so none of that
 * needs a lock. Other threads (the IPC thread) talk to the worker only through
 * the fields marked "shared" below, using atomics plus a wake-up eventfd:
 *
 *   IPC thread                          worker thread
 *   ──────────                          ─────────────
 *   flux_dl_pause()  ── want_paused ──▶ stops segments, saves resume, PAUSED
 *   flux_dl_cancel() ── stop_mode   ──▶ stops segments, (deletes files), exits
 *   flux_dl_snapshot() ◀─ atomics ───── downloaded_bytes, speed, state …
 *
 * Downloads, once started, are never freed by their own thread: the worker
 * just marks itself finished, and the IPC thread (the only owner of the
 * registry) joins and frees it later. That makes "look up an id, then use the
 * pointer" safe without reference counting.
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
#include <sys/socket.h>

#define FLUX_DL_MAX_SEGMENTS      FLUX_MAX_SEGMENTS
#define FLUX_EPOLL_MAX_EVENTS     128
#define FLUX_PROGRESS_INTERVAL_MS 250      /* progress event + speed sample period */
#define FLUX_RESUME_SAVE_MS       3000     /* resume-file checkpoint period        */
#define FLUX_MIN_SEGMENT_BYTES    (256 * 1024)  /* don't split files below this    */
#define FLUX_DL_RETAIN_MS         (10 * 60 * 1000) /* keep finished downloads queryable */
#define FLUX_REG_BUCKETS          1024

/* ── Download state ───────────────────────────────────────────────────────── */
typedef enum {
    DL_IDLE        = 0,
    DL_PROBING     = 1,   /* resolving, redirects, size + range discovery */
    DL_RUNNING     = 2,
    DL_PAUSED      = 3,
    DL_COMPLETE    = 4,
    DL_ERROR       = 5,
    DL_CANCELLED   = 6,
} dl_state_t;

/* Why the worker was asked to stop (flux_download_t::stop_mode) */
typedef enum {
    DL_STOP_NONE   = 0,
    DL_STOP_KEEP   = 1,   /* shutdown: keep the partial file + resume state */
    DL_STOP_DELETE = 2,   /* user cancel: delete the partial file + resume state */
} dl_stop_mode_t;

struct flux_download;
typedef void (*flux_event_cb)(struct flux_download *dl, flux_msg_type_t event);

/* ── Download object ──────────────────────────────────────────────────────── */
typedef struct flux_download {
    uint32_t        id;
    uint32_t        state;              /* dl_state_t — shared, __atomic access */

    /* Request parameters (immutable after creation) */
    char            url[FLUX_MAX_URL];  /* the URL as given                  */
    char            file_path[FLUX_MAX_PATH];
    char            part_path[FLUX_MAX_PATH + 16];
    uint8_t         want_segs;          /* segments requested by the client   */
    uint8_t         cksum_type;         /* flux_cksum_type_t                  */
    uint8_t         cksum_expected[FLUX_CKSUM_MAX_LEN];

    /* Where segments connect: the URL after following redirects (worker-owned) */
    http_url_t      origin;             /* the URL as given, parsed            */
    http_url_t      target;
    uint32_t        target_gen;         /* bumped whenever `target` changes    */
    uint16_t        redirects;          /* consecutive redirects without a successful response */
    uint8_t         use_tls;
    struct sockaddr_storage addr;       /* pinned peer address (skips DNS)    */
    socklen_t       addr_len;

    /* Output file */
    int             file_fd;

    /* Remote file metadata (from the probe) */
    uint64_t        total_size;         /* 0 if the server doesn't say        */
    uint8_t         accept_ranges;
    uint8_t         empty;              /* the remote file is known to be 0 bytes */
    uint8_t         owns_file;          /* this download created/opened the output file
                                         * (so a cancel may delete it)            */
    uint64_t        validator;          /* hash of Last-Modified/ETag + size; 0 = none */

    /* Segments */
    flux_segment_t *segs[FLUX_DL_MAX_SEGMENTS];
    uint8_t         num_segs;
    uint8_t         num_segs_active;    /* shared (snapshot only)             */

    /* Probe connection handed to the first segment when the server ignores
     * Range (single-stream download): see flux_seg_adopt(). */
    flux_conn_t    *probe_conn;
    http_response_t *probe_resp;

    /* Progress — shared: workers add, the IPC thread reads (__atomic) */
    uint64_t        downloaded_bytes;   /* received = on disk + buffered      */
    uint32_t        speed_bps;
    uint64_t        speed_mark_bytes;   /* worker-only speed sampling state   */
    uint64_t        speed_mark_ms;
    uint64_t        last_progress_ms;
    uint64_t        started_ms;
    uint64_t        finished_ms;

    /* Bandwidth */
    flux_bucket_t   bw_bucket;          /* per-download limit                 */
    flux_bucket_t  *global_bucket;      /* daemon-wide limit (owned by registry) */

    /* Result of the post-download verification */
    uint8_t         cksum_ok;           /* 1 only if a checksum was given AND matched */
    uint8_t         cksum_actual[FLUX_CKSUM_MAX_LEN];

    /* Resume (heap, worker-owned) */
    flux_resume_t  *resume;
    uint64_t        last_resume_save_ms;
    uint64_t        saved_written;      /* bytes on disk at the last checkpoint */

    /* Event loop */
    int             epoll_fd;
    int             wake_fd;            /* eventfd: IPC thread → worker       */

    /* Control — shared (__atomic). Pause is a small handshake so that a
     * pause and a resume racing each other cannot both "win":
     *   0 = run       1 = pause requested (worker has not acted yet)
     *   2 = pause committed (worker is stopping; only a relaunch undoes it)
     * The worker moves 1→2 with a compare-exchange before it acts, and
     * flux_dl_resume() moves 1→0 the same way: exactly one of them succeeds. */
    int             want_paused;
    int             stop_mode;          /* dl_stop_mode_t                     */

    /* Failure details (valid once state == DL_ERROR) */
    flux_error_t    err_code;
    char            err_msg[256];

    /* Lifecycle */
    pthread_t       thread;
    uint8_t         thread_started;     /* 1 only after pthread_create() SUCCEEDS
                                         * — joining a never-created pthread_t is
                                         * undefined behaviour (it segfaulted on
                                         * newer glibc) so this is what every
                                         * join is gated on. */
    int             finished;           /* worker has exited (__atomic)       */
    uint8_t         joined;
    uint64_t        finished_at_ms;     /* when the IPC thread first saw `finished` */
    pthread_mutex_t lock;               /* guards the condvar below           */
    pthread_cond_t  cv;                 /* signalled on every state change    */

    /* Events go out through this callback (called from the worker thread) */
    flux_event_cb   on_event;

    struct flux_download *hash_next;    /* registry chaining                  */
} flux_download_t;

/* ── Registry ─────────────────────────────────────────────────────────────── */
typedef struct {
    flux_download_t *buckets[FLUX_REG_BUCKETS];
    int              count;
    flux_bucket_t    global_bw;
    pthread_mutex_t  lock;
} flux_registry_t;

/* One-time init of the global registry (also initialises TLS). */
int  flux_registry_init(flux_registry_t *reg);

/* Stop every download (partial files and resume state are kept), join the
 * workers and free everything. */
void flux_registry_destroy(flux_registry_t *reg);

/* Look up by id. Returns NULL if not found. Only the thread that owns the
 * registry (the IPC thread) may use the returned pointer. */
flux_download_t *flux_registry_find(flux_registry_t *reg, uint32_t id);

/* Visit every download (for CMD_STATUS_ALL). */
void flux_registry_foreach(flux_registry_t *reg,
                           void (*fn)(flux_download_t *dl, void *ctx), void *ctx);

/* Join and free downloads whose worker has exited and whose retention time is
 * over. Cheap; call it from the IPC loop about once a second. */
void flux_registry_reap(flux_registry_t *reg, uint64_t now_ms);

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/* Validate the request and register a new download WITHOUT starting it.
 * Returns NULL (with the thread-local error set) on a bad URL/segment count,
 * out-of-memory, or an id that is still active. A finished download with the
 * same id is replaced. The caller replies to the client, then calls
 * flux_dl_launch() — so the client always sees the acknowledgement before any
 * event for this download. */
flux_download_t *flux_dl_start(flux_registry_t   *reg,
                               uint32_t           id,
                               const char        *url,
                               const char        *dest_path,
                               uint8_t            num_segs,
                               uint64_t           bw_limit,
                               flux_cksum_type_t  cksum_type,
                               const uint8_t     *cksum_expected,
                               flux_event_cb      on_event);

/* Spawn the worker thread. The worker probes the URL, then downloads; failures
 * are reported through on_event(FLUX_RSP_ERROR). On failure to spawn, the
 * download is put in DL_ERROR and 0 is returned after emitting that event
 * synchronously; returns -1 only if `dl` is NULL. */
int  flux_dl_launch(flux_download_t *dl);

/* Pause a download: the worker flushes buffers, closes connections and writes
 * its resume state. Waits up to ~2 s for that to happen, then returns. */
int  flux_dl_pause(flux_download_t *dl);

/* Resume a paused download. */
int  flux_dl_resume(flux_download_t *dl);

/* Cancel: stop the worker, delete the partial file and resume state (a
 * finished download's output file is never touched), and release the download
 * when the worker has exited. If the worker is stuck in a blocking call
 * (DNS) it is released later by flux_registry_reap(). */
int  flux_dl_cancel(flux_registry_t *reg, flux_download_t *dl);

/* Fill an IPC status record from the shared counters (any thread). */
void flux_dl_snapshot(const flux_download_t *dl, flux_rsp_status_t *out);

/* Seconds until completion at the current speed; UINT32_MAX if unknown. */
uint32_t flux_dl_eta(const flux_download_t *dl);

/* ── Internal API used by segment.c ───────────────────────────────────────── */

/* Add (or, when negative, remove) received bytes. Thread-safe. */
void flux_dl_add_progress(flux_download_t *dl, int64_t delta);

/* Charge `bytes` that were just read against the per-download and global
 * limits. Returns how many ms to stop reading (0 = keep going). */
uint32_t flux_dl_charge(flux_download_t *dl, size_t bytes);

/* Largest single read that keeps the limiter smooth (SIZE_MAX if unlimited). */
size_t   flux_dl_read_cap(flux_download_t *dl);

/* A segment was redirected: resolve `location` against the current target and
 * make it the new target for future connections. 0 on success. */
int      flux_dl_note_redirect(flux_download_t *dl, const char *location);
