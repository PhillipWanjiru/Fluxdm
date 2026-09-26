/* FluxDM — core/src/main.c
 * Entry point for the C core daemon process.
 * Handles signals, initialises the registry, and runs the IPC server.
 * The Java daemon starts this process on boot and communicates via
 * the Unix socket at /tmp/fluxdm.sock.
 *
 * Usage:
 *   fluxdm-core [--sock <path>] [--limit <bytes_per_sec>] [--log <level>]
 */

#include "../include/downloader.h"
#include "../include/errors.h"
#include "../include/protocol/https.h"
#include "ipc/socket_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <errno.h>

/* ── Globals ──────────────────────────────────────────────────────────────── */
static flux_registry_t   g_registry;
static flux_ipc_server_t g_ipc_server;
static volatile int      g_shutdown = 0;

/* ── Signal handler ───────────────────────────────────────────────────────── */
static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
    flux_ipc_stop(&g_ipc_server);
}

/* ── Raise file descriptor limit ──────────────────────────────────────────── */
static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* ── Argument parser ──────────────────────────────────────────────────────── */
typedef struct {
    uint64_t global_limit;   /* bytes/sec, 0 = unlimited */
    int      log_level;      /* 0=quiet 1=info 2=debug   */
} flux_args_t;

static void parse_args(int argc, char **argv, flux_args_t *args) {
    memset(args, 0, sizeof(*args));
    args->log_level = 1;  /* default: info */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            args->global_limit = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            args->log_level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "FluxDM Core Daemon\n"
                "Usage: fluxdm-core [options]\n"
                "  --limit  <bytes/sec>  Global bandwidth cap (0=unlimited)\n"
                "  --log    <0|1|2>      Log level: 0=quiet 1=info 2=debug\n"
                "  --help                Show this message\n"
            );
            exit(0);
        } else {
            fprintf(stderr, "[fluxdm-core] unknown arg: %s\n", argv[i]);
        }
    }
}

/* ── Banner ───────────────────────────────────────────────────────────────── */
static void print_banner(void) {
    fprintf(stderr,
        "\n"
        "  ███████╗██╗     ██╗   ██╗██╗  ██╗██████╗ ███╗   ███╗\n"
        "  ██╔════╝██║     ██║   ██║╚██╗██╔╝██╔══██╗████╗ ████║\n"
        "  █████╗  ██║     ██║   ██║ ╚███╔╝ ██║  ██║██╔████╔██║\n"
        "  ██╔══╝  ██║     ██║   ██║ ██╔██╗ ██║  ██║██║╚██╔╝██║\n"
        "  ██║     ███████╗╚██████╔╝██╔╝ ██╗██████╔╝██║ ╚═╝ ██║\n"
        "  ╚═╝     ╚══════╝ ╚═════╝ ╚═╝  ╚═╝╚═════╝ ╚═╝     ╚═╝\n"
        "\n"
        "  C Core Daemon — Phase 1\n"
        "  IPC socket : " FLUX_IPC_SOCK_PATH "\n"
        "\n"
    );
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    flux_args_t args;
    parse_args(argc, argv, &args);

    if (args.log_level > 0) print_banner();

    /* Raise open-file limit — each segment needs a socket */
    raise_fd_limit();

    /* Block SIGPIPE globally — we handle broken pipes via return codes */
    signal(SIGPIPE, SIG_IGN);

    /* Graceful shutdown on SIGTERM / SIGINT */
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Initialise OpenSSL and download registry */
    if (flux_registry_init(&g_registry) < 0) {
        fprintf(stderr, "[fluxdm-core] registry init failed: %s\n",
                flux_last_error_str());
        return 1;
    }

    /* Apply global bandwidth limit if specified */
    if (args.global_limit > 0) {
        flux_bw_set_rate(&g_registry.global_bw, args.global_limit);
        if (args.log_level > 0)
            fprintf(stderr, "[fluxdm-core] global limit: %llu bytes/sec\n",
                    (unsigned long long)args.global_limit);
    }

    /* Run IPC server — blocks until SIGTERM/SIGINT or CMD_SHUTDOWN */
    int rc = flux_ipc_server_run(&g_ipc_server, &g_registry);

    if (args.log_level > 0)
        fprintf(stderr, "[fluxdm-core] shutting down...\n");

    /* Teardown: pause all active downloads and flush resume state */
    flux_registry_destroy(&g_registry);

    if (args.log_level > 0)
        fprintf(stderr, "[fluxdm-core] goodbye.\n");

    return rc < 0 ? 1 : 0;
}
