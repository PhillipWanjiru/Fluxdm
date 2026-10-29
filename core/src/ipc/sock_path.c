/* FluxDM — core/src/ipc/sock_path.c
 * See sock_path.h for the rules. */
#include "sock_path.h"
#include "../../include/errors.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SOCK_NAME      "core.sock"
#define RUN_SUBDIR     "fluxdm"          /* inside $XDG_RUNTIME_DIR              */
#define HOME_SUBDIR    ".fluxdm"         /* inside $HOME ...                     */
#define HOME_RUN       "run"             /* ... and then this one                */
#define SUN_PATH_BYTES 108               /* sizeof(((struct sockaddr_un *)0)->sun_path), NUL included */
#define DIR_BYTES      4096

/* A directory that belongs to us and that nobody else can enter. A final symbolic link is followed:
 * a runtime directory may legitimately be one. */
static int is_private_dir(const char *dir) {
    struct stat sb;
    return stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode) && sb.st_uid == geteuid() && (sb.st_mode & 077) == 0;
}

/* Create `dir` (mode 0700) unless it exists, then look at what is there. The parent must exist.
 * The checks run on an open descriptor, so nothing can be swapped in between the check and the fix. */
static int make_private_dir(const char *dir) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        flux_set_error(FLUX_ERR_IPC_BIND, "cannot create %s: %s", dir, strerror(errno));
        return -1;
    }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        flux_set_error(FLUX_ERR_IPC_BIND, "%s is not a plain directory (%s); remove it, or give --sock",
                       dir, strerror(errno));
        return -1;
    }
    struct stat sb;
    int rc = -1;
    if (fstat(fd, &sb) != 0) {
        flux_set_error(FLUX_ERR_IPC_BIND, "cannot inspect %s: %s", dir, strerror(errno));
    } else if (sb.st_uid != geteuid()) {
        flux_set_error(FLUX_ERR_IPC_BIND, "%s belongs to another user; remove it, or give --sock", dir);
    } else if ((sb.st_mode & 077) != 0 && fchmod(fd, 0700) != 0) {
        flux_set_error(FLUX_ERR_IPC_BIND, "cannot close %s to other users: %s", dir, strerror(errno));
    } else {
        rc = 0;
    }
    close(fd);
    return rc;
}

int flux_ipc_resolve_sock_path(const char *xdg_runtime_dir, const char *home, char *out, size_t cap) {
    char dir[DIR_BYTES], base[DIR_BYTES] = "";
    int  use_xdg = xdg_runtime_dir && xdg_runtime_dir[0] == '/' && is_private_dir(xdg_runtime_dir);

    if (use_xdg) {
        if (snprintf(dir, sizeof(dir), "%s/%s", xdg_runtime_dir, RUN_SUBDIR) >= (int)sizeof(dir)) dir[0] = '\0';
    } else {
        if (xdg_runtime_dir && xdg_runtime_dir[0] != '\0')
            flux_log(1, "XDG_RUNTIME_DIR (%s) is not an absolute path to a directory of yours that is closed to "
                        "others; using ~/%s/%s instead", xdg_runtime_dir, HOME_SUBDIR, HOME_RUN);
        if (!home || home[0] != '/') {
            flux_set_error(FLUX_ERR_IPC_BIND, "cannot tell where your home directory is; give --sock");
            return -1;
        }
        if (snprintf(base, sizeof(base), "%s/%s", home, HOME_SUBDIR) >= (int)sizeof(base)
            || snprintf(dir, sizeof(dir), "%s/%s", base, HOME_RUN) >= (int)sizeof(dir)) dir[0] = '\0';
    }

    /* The whole path has to fit sockaddr_un: find out before anything is created. */
    size_t need = strlen(dir) + 1 + strlen(SOCK_NAME);
    if (dir[0] == '\0' || need >= cap || need >= SUN_PATH_BYTES) {
        flux_set_error(FLUX_ERR_IPC_BIND, "the default socket path would be too long (max %d characters); give --sock",
                       SUN_PATH_BYTES - 1);
        return -1;
    }

    if (!use_xdg && mkdir(base, 0700) != 0 && errno != EEXIST) {      /* the daemon keeps its data here, too */
        flux_set_error(FLUX_ERR_IPC_BIND, "cannot create %s: %s", base, strerror(errno));
        return -1;
    }
    if (make_private_dir(dir) != 0) return -1;

    snprintf(out, cap, "%s/%s", dir, SOCK_NAME);
    return 0;
}

int flux_ipc_default_sock_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    char        buf[1024];
    struct passwd pw, *res = NULL;
    if (!home || home[0] != '/') {                            /* a daemon may run without HOME */
        if (getpwuid_r(geteuid(), &pw, buf, sizeof(buf), &res) == 0 && res) home = res->pw_dir;
        else home = NULL;
    }
    return flux_ipc_resolve_sock_path(getenv("XDG_RUNTIME_DIR"), home, out, cap);
}
