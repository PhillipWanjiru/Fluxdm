/* FluxDM — core/src/resume.c */

#include "../include/resume.h"
#include "../include/errors.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

void flux_resume_path(const char *file_path, char *out, size_t out_len) {
    snprintf(out, out_len, "%s.fluxpart", file_path);
}

int flux_resume_save(const flux_resume_t *r, const char *part_path) {
    /* Write to a temp file, then atomically rename */
    char tmp_path[4200];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", part_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        flux_set_syserror("resume_save: fopen");
        return -1;
    }

    /* Header */
    if (fwrite(&r->hdr, sizeof(r->hdr), 1, f) != 1) goto write_err;

    /* Segment entries */
    if (fwrite(r->segs, sizeof(flux_resume_seg_t),
               r->hdr.num_segments, f) != r->hdr.num_segments) goto write_err;

    /* URL (uint16_t len + data) */
    uint16_t url_len  = (uint16_t)strnlen(r->url,  FLUX_RESUME_MAX_URL - 1);
    uint16_t path_len = (uint16_t)strnlen(r->path, FLUX_RESUME_MAX_URL - 1);

    if (fwrite(&url_len, sizeof(url_len), 1, f) != 1) goto write_err;
    if (fwrite(r->url, 1, url_len, f) != url_len) goto write_err;

    /* Save path */
    if (fwrite(&path_len, sizeof(path_len), 1, f) != 1) goto write_err;
    if (fwrite(r->path, 1, path_len, f) != path_len) goto write_err;

    /* Flush to OS and sync to disk before rename */
    if (fflush(f) != 0) goto write_err;
    if (fsync(fileno(f)) < 0) goto write_err;
    fclose(f);

    /* Atomic rename */
    if (rename(tmp_path, part_path) < 0) {
        flux_set_syserror("resume_save: rename");
        unlink(tmp_path);
        return -1;
    }
    return 0;

write_err:
    flux_set_syserror("resume_save: fwrite");
    fclose(f);
    unlink(tmp_path);
    return -1;
}

int flux_resume_load(flux_resume_t *r, const char *part_path) {
    FILE *f = fopen(part_path, "rb");
    if (!f) {
        /* Not an error — just means no prior state */
        memset(r, 0, sizeof(*r));
        return -1;
    }

    if (fread(&r->hdr, sizeof(r->hdr), 1, f) != 1) goto corrupt;

    if (r->hdr.magic != FLUX_RESUME_MAGIC) goto corrupt;
    if (r->hdr.version != FLUX_RESUME_VER) goto corrupt;
    if (r->hdr.num_segments > FLUX_RESUME_MAX_SEG) goto corrupt;

    if (r->hdr.num_segments > 0) {
        if (fread(r->segs, sizeof(flux_resume_seg_t),
                  r->hdr.num_segments, f) != r->hdr.num_segments) goto corrupt;
    }

    uint16_t url_len = 0, path_len = 0;

    if (fread(&url_len, sizeof(url_len), 1, f) != 1) goto corrupt;
    if (url_len >= FLUX_RESUME_MAX_URL) goto corrupt;
    if (fread(r->url, 1, url_len, f) != url_len) goto corrupt;
    r->url[url_len] = '\0';

    if (fread(&path_len, sizeof(path_len), 1, f) != 1) goto corrupt;
    if (path_len >= FLUX_RESUME_MAX_URL) goto corrupt;
    if (fread(r->path, 1, path_len, f) != path_len) goto corrupt;
    r->path[path_len] = '\0';

    fclose(f);
    return 0;

corrupt:
    fclose(f);
    flux_set_error(FLUX_ERR_DISK_READ, "resume file corrupt: %s", part_path);
    memset(r, 0, sizeof(*r));
    return -1;
}

void flux_resume_delete(const char *part_path) {
    unlink(part_path);
}
