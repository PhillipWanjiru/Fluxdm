/* FluxDM — core/src/main.c
 * Entry point of the C core daemon process.
 * The Java daemon starts this process on boot and talks to it over a Unix
 * socket (default /tmp/fluxdm.sock, mode 0600).
 *
 * Usage:
 *   fluxdm-core [--sock <path>] [--limit <bytes_per_sec>] [--log <0|1|2>]
 *               [--patience <seconds>]
 */

#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/hostgate.h"
#include "../include/version.h"
#include "ipc/socket_server.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static flux_registry_t   g_registry;
static flux_ipc_server_t g_ipc_server;

/* ── Signals ──────────────────────────────────────────────────────────────── */

/* Only async-signal-safe work: flux_ipc_stop() stores a flag and write()s an eventfd. */
static void handle_signal(int sig) {
    (void)sig;
    flux_ipc_stop(&g_ipc_server);
}

/* Every segment needs a socket: lift the soft open-file limit to the hard one. */
static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = rl.rlim_max;
    if (want == RLIM_INFINITY || want > 1048576) want = 1048576;
    if (rl.rlim_cur >= want) return;
    rl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {          /* e.g. above fs.nr_open */
        rl.rlim_cur = 65536 < rl.rlim_max ? 65536 : rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* ── Arguments ────────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t    global_limit;    /* bytes/sec, 0 = unlimited */
    int         log_level;       /* 0 quiet, 1 info, 2 debug */
    const char *sock_path;       /* NULL = default           */
    int         patience_set;    /* --patience was given     */
    uint32_t    patience_ms;
} flux_args_t;

static void usage(FILE *out) {
    fprintf(out,
        "FluxDM core daemon %s\n"
        "Usage: fluxdm-core [options]\n"
        "  --sock  <path>        Unix socket to listen on (default " FLUX_IPC_SOCK_PATH ")\n"
        "  --limit <bytes/sec>   Global bandwidth cap, 0 = unlimited (default 0)\n"
        "  --log   <0|1|2>       Log level: 0 quiet, 1 info, 2 debug (default 1)\n"
        "  --patience <seconds>  How long a download keeps waiting for a server that answers\n"
        "                        429 / 503 while it serves none of our connections before\n"
        "                        it fails, 0 = fail at the first refusal (default 300)\n"
        "  --version             Print the version and exit\n"
        "  --help                Show this message\n", FLUXDM_VERSION);
}

/* A plain decimal number: no sign, no blanks, no trailing junk, no empty string
 * (strtoull alone accepts "" as 0 and "-1" as 2^64-1). */
static int parse_uint(const char *s, unsigned long long *out) {
    if (*s < '0' || *s > '9') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') return -1;
    *out = v;
    return 0;
}

static int parse_args(int argc, char **argv, flux_args_t *args) {
    memset(args, 0, sizeof(*args));
    args->log_level = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int has_value = i + 1 < argc;

        if (strcmp(a, "--limit") == 0 && has_value) {
            unsigned long long v;
            if (parse_uint(argv[++i], &v) < 0) { fprintf(stderr, "fluxdm-core: bad --limit value\n"); return -1; }
            args->global_limit = (uint64_t)v;
        } else if (strcmp(a, "--log") == 0 && has_value) {
            args->log_level = atoi(argv[++i]);
            if (args->log_level < 0) args->log_level = 0;
            if (args->log_level > 2) args->log_level = 2;
        } else if (strcmp(a, "--patience") == 0 && has_value) {
            unsigned long long v;
            if (parse_uint(argv[++i], &v) < 0 || v > 86400ull) {
                fprintf(stderr, "fluxdm-core: bad --patience value (seconds, 0 to 86400)\n");
                return -1;
            }
            args->patience_set = 1;
            args->patience_ms  = (uint32_t)(v * 1000u);
        } else if (strcmp(a, "--sock") == 0 && has_value) {
            args->sock_path = argv[++i];
        } else if (strcmp(a, "--version") == 0) {
            printf("fluxdm-core %s\n", FLUXDM_VERSION);
            exit(0);
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "fluxdm-core: unknown or incomplete argument: %s\n", a);
            usage(stderr);
            return -1;
        }
    }
    return 0;
}

static void print_banner(const char *sock_path) {
    fprintf(stderr,
        "\n"
        "  ███████╗██╗     ██╗   ██╗██╗  ██╗██████╗ ███╗   ███╗\n"
        "  ██╔════╝██║     ██║   ██║╚██╗██╔╝██╔══██╗████╗ ████║\n"
        "  █████╗  ██║     ██║   ██║ ╚███╔╝ ██║  ██║██╔████╔██║\n"
        "  ██╔══╝  ██║     ██║   ██║ ██╔██╗ ██║  ██║██║╚██╔╝██║\n"
        "  ██║     ███████╗╚██████╔╝██╔╝ ██╗██████╔╝██║ ╚═╝ ██║\n"
        "  ╚═╝     ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚═════╝ ╚═╝     ╚═╝\n"
        "\n"
        "  C core daemon %s\n"
        "  IPC socket : %s\n"
        "\n", FLUXDM_VERSION, sock_path);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    flux_args_t args;
    if (parse_args(argc, argv, &args) < 0) return 2;

    flux_log_set_level(args.log_level);
    if (args.log_level > 0) print_banner(args.sock_path ? args.sock_path : FLUX_IPC_SOCK_PATH);

    if (args.patience_set) flux_gate_set_patience_ms(args.patience_ms);
    raise_fd_limit();
    signal(SIGPIPE, SIG_IGN);                   /* broken pipes are reported through return codes */

    if (flux_registry_init(&g_registry) < 0) {
        fprintf(stderr, "[fluxdm-core] cannot initialise: %s\n", flux_last_error_str());
        return 1;
    }
    if (args.global_limit > 0) {
        flux_bw_set_rate(&g_registry.global_bw, args.global_limit);
        flux_log(1, "global limit: %llu bytes/sec", (unsigned long long)args.global_limit);
    }

    if (flux_ipc_server_init(&g_ipc_server, &g_registry, args.sock_path) < 0) {
        fprintf(stderr, "[fluxdm-core] cannot start the IPC server: %s\n", flux_last_error_str());
        flux_ipc_server_close(&g_ipc_server);
        flux_registry_destroy(&g_registry);
        return 1;
    }

    /* Handlers go in only now: flux_ipc_stop() needs the server's wake-up fd. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    int rc = flux_ipc_server_run(&g_ipc_server);

    flux_log(1, "shutting down...");
    flux_registry_destroy(&g_registry);         /* stops every download, keeping partial files and resume state */
    flux_ipc_server_close(&g_ipc_server);
    flux_log(1, "goodbye.");

    return rc < 0 ? 1 : 0;
}
