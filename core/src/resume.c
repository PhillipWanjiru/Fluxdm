/* FluxDM — core/src/resume.c */

#include "../include/resume.h"
#include "../include/errors.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

void flux_resume_path(const char *file_path, char *out, size_t out_len) {
    snprintf(out, out_len, "%s.fluxpart", file_path);
}

size_t flux_resume_redact_url(const char *url, char *out, size_t out_len) {
    if (out_len == 0) return 0;
    const char *sep = strstr(url, "://");
    if (sep) {
        const char *auth     = sep + 3;
        const char *auth_end = auth + strcspn(auth, "/?#");
        const char *at       = NULL;
        for (const char *c = auth; c < auth_end; c++) if (*c == '@') at = c;
        if (at) {
            snprintf(out, out_len, "%.*s%s", (int)(auth - url), url, at + 1);
            return strlen(out);
        }
    }
    snprintf(out, out_len, "%s", url);
    return strlen(out);
}

int flux_resume_save(const flux_resume_t *r, const char *part_path) {
    /* Write to a temp file, then atomically rename */
    char tmp_path[4200];
    int  n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", part_path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        errno = ENAMETOOLONG;
        flux_set_syserror("resume_save: path");
        return -1;
    }

    /* A temp file left behind by a crash is removed first, and O_EXCL then makes sure the file we write is one we
     * created just now: not one that somebody else put there, and (O_NOFOLLOW) not a link to a file of ours that
     * they would like to see overwritten. Mode 0600: the contents are nobody else's business. */
    unlink(tmp_path);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        flux_set_syserror("resume_save: open");
        return -1;
    }
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        flux_set_syserror("resume_save: fdopen");
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    /* Header */
    if (fwrite(&r->hdr, sizeof(r->hdr), 1, f) != 1) goto write_err;

    /* Segment entries */
    if (fwrite(r->segs, sizeof(flux_resume_seg_t),
               r->hdr.num_segments, f) != r->hdr.num_segments) goto write_err;

    /* URL (uint16_t len + data) */
    char url[FLUX_RESUME_MAX_URL];
    flux_resume_redact_url(r->url, url, sizeof(url));
    uint16_t url_len  = (uint16_t)strnlen(url,     FLUX_RESUME_MAX_URL - 1);
    uint16_t path_len = (uint16_t)strnlen(r->path, FLUX_RESUME_MAX_URL - 1);

    if (fwrite(&url_len, sizeof(url_len), 1, f) != 1) goto write_err;
    if (fwrite(url, 1, url_len, f) != url_len) goto write_err;

    /* Save path */
    if (fwrite(&path_len, sizeof(path_len), 1, f) != 1) goto write_err;
    if (fwrite(r->path, 1, path_len, f) != path_len) goto write_err;

    /* Flush to OS and sync to disk before rename */
    if (fflush(f) != 0) goto write_err;
    if (fsync(fileno(f)) < 0) goto write_err;
    if (fclose(f) != 0) {                      /* the descriptor is closed even when this fails */
        flux_set_syserror("resume_save: close");
        unlink(tmp_path);
        return -1;
    }

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
    /* O_NOFOLLOW: a link is not a resume file. O_NONBLOCK: opening a FIFO that nobody writes to would wait for
     * ever; the file-type check below then turns it away. */
    int fd = open(part_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        /* Not an error — just means no prior state */
        memset(r, 0, sizeof(*r));
        return -1;
    }
    struct stat sb;
    FILE *f = NULL;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || !(f = fdopen(fd, "rb"))) {
        if (!f) close(fd);
        flux_set_error(FLUX_ERR_DISK_READ, "resume file corrupt: %s", part_path);
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
