/* FluxDM — core/include/resume.h
 * Persistent resume state written to <filename>.fluxpart
 * Format: fixed header + per-segment entries + variable-length strings.
 * Designed to survive crashes: header is written last (atomic rename).
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define FLUX_RESUME_MAGIC   0x464C5850u   /* "FLXP" */
#define FLUX_RESUME_VER     1u
#define FLUX_RESUME_MAX_URL 4096
#define FLUX_RESUME_MAX_SEG 64

/* ── On-disk segment entry (fixed 32 bytes) ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t range_start;
    uint64_t range_end;         /* UINT64_MAX = open-ended */
    uint64_t written;           /* bytes confirmed on disk */
    uint8_t  status;            /* 0=pending 1=complete    */
    uint8_t  _pad[7];
} flux_resume_seg_t;

/* ── On-disk header (fixed 128 bytes) ────────────────────────────────────────*/
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t download_id;
    uint32_t num_segments;
    uint64_t total_size;        /* 0 if unknown            */
    uint8_t  checksum_type;     /* 0=none 1=MD5 2=SHA256   */
    uint8_t  checksum[32];
    uint64_t validator;         /* hash of the remote file's Last-Modified/ETag +
                                 * size at download start; 0 = none (older files).
                                 * A resume is refused if it no longer matches. */
    uint8_t  _pad[63];
    /* followed by: flux_resume_seg_t[num_segments]
     *              uint16_t url_len, char url[url_len]
     *              uint16_t path_len, char path[path_len]  */
} flux_resume_hdr_t;

#ifdef __cplusplus
static_assert(sizeof(flux_resume_hdr_t) == 128, "resume header must be 128 bytes");
static_assert(sizeof(flux_resume_seg_t) == 32,  "resume segment must be 32 bytes");
#else
_Static_assert(sizeof(flux_resume_hdr_t) == 128, "resume header must be 128 bytes");
_Static_assert(sizeof(flux_resume_seg_t) == 32,  "resume segment must be 32 bytes");
#endif

/* ── API ──────────────────────────────────────────────────────────────────── */

/* In-memory representation loaded/saved by the functions below */
typedef struct {
    flux_resume_hdr_t hdr;
    flux_resume_seg_t segs[FLUX_RESUME_MAX_SEG];
    char url[FLUX_RESUME_MAX_URL];
    char path[FLUX_RESUME_MAX_URL];
} flux_resume_t;

/* Write state atomically: write to .fluxpart.tmp, then rename over .fluxpart.
 *
 * The file is created mode 0600 and is never opened through a link: it sits next to the download, in a folder other
 * accounts may be able to read or write, and it names the URL. The URL is written without its "user:password@" part
 * (see flux_resume_redact_url), so a password never reaches the disk through here. */
int  flux_resume_save(const flux_resume_t *r, const char *part_path);

/* Load and validate an existing .fluxpart file.
 * Returns 0 on success, -1 if missing/corrupt (start fresh). A link, a FIFO, a device or a directory in its place
 * is treated as corrupt, and is never followed or waited on. */
int  flux_resume_load(flux_resume_t *r, const char *part_path);

/* `url` without the user name and password of its authority ("http://u:p@host/f" -> "http://host/f"). The user
 * information is what precedes the LAST '@' of the authority, the rule the URL parser uses. Copies at most
 * out_len-1 bytes, always ends the output with NUL (when out_len > 0) and returns its length. Resume files hold
 * URLs in this form, and a stored URL is compared with the current one in this form, so a file written by an older
 * version, which kept the password, still matches. */
size_t flux_resume_redact_url(const char *url, char *out, size_t out_len);

/* Delete the .fluxpart file after a successful completed download. */
void flux_resume_delete(const char *part_path);

/* Build part_path from the destination file path (appends ".fluxpart"). */
void flux_resume_path(const char *file_path, char *out, size_t out_len);
