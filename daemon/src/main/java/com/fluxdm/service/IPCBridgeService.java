/* FluxDM — service/IPCBridgeService.java
 * Owns the relationship with the fluxdm-core process: finds or starts it,
 * connects CoreIPCClient to it, keeps it connected, and shuts it down
 * cleanly when the daemon stops.
 *
 *  - Start: if a core is already serving the socket (the daemon was restarted
 *    while the core kept running, or someone started one by hand) it is adopted
 *    and downloads keep going without a hiccup. Otherwise one is spawned with
 *    --sock, so the daemon and the core can never disagree about the path; the
 *    log level, the bandwidth cap and the rate-limit patience (how long the core
 *    waits for a server that answers 429 / 503) come from application.properties.
 *    The socket file is never deleted from here: the core refuses a socket that
 *    a live process serves and replaces a stale one itself.
 *
 *  - Supervise: a background thread checks the connection every second. If the
 *    core crashed or the connection dropped it restarts the core (when it is
 *    ours) and reconnects, backing off while that keeps failing. Re-synchronising
 *    the downloads after a reconnect is DownloadService's job — it hears about
 *    the new connection from CoreIPCClient.
 *
 *  - Stop: only a core this service spawned is shut down (CMD_SHUTDOWN, then
 *    SIGTERM, then SIGKILL as a last resort). An adopted core is left running.
 */
package com.fluxdm.service;

import com.fluxdm.ipc.CoreIPCClient;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

@Service
public class IPCBridgeService {

    private static final Logger log = LoggerFactory.getLogger(IPCBridgeService.class);

    private static final long FIRST_RETRY_MS = 500;
    private static final long MAX_RETRY_MS   = 15_000;
    private static final long STABLE_MS      = 10_000;   /* connected this long = the last crash is forgiven */
    private static final int  MAX_PATIENCE_S = 86_400;   /* the core's own upper bound for --patience */

    private final CoreIPCClient ipc;
    private final String        coreBinaryPath;
    private final int           logLevel;
    private final long          startupTimeoutMs;
    private final long          globalLimitBps;
    private final int           patienceSeconds;

    private final Object connectLock = new Object();

    private volatile Process coreProcess;           /* the one WE spawned; null if adopted or not running */
    private volatile boolean stopping;
    private final AtomicInteger restarts = new AtomicInteger();

    private ScheduledExecutorService supervisor;
    private long retryDelayMs = FIRST_RETRY_MS;
    private long nextAttemptMs;
    private long connectedSinceMs;

    public IPCBridgeService(CoreIPCClient ipc,
                            @Value("${fluxdm.core.binary-path}") String coreBinaryPath,
                            @Value("${fluxdm.core.log-level:1}") int logLevel,
                            @Value("${fluxdm.core.startup-timeout-ms:5000}") long startupTimeoutMs,
                            @Value("${fluxdm.core.global-limit-bps:0}") long globalLimitBps,
                            @Value("${fluxdm.core.patience-seconds:300}") int patienceSeconds) {
        this.ipc              = ipc;
        this.coreBinaryPath   = coreBinaryPath;
        this.logLevel         = logLevel;
        this.startupTimeoutMs = startupTimeoutMs;
        this.globalLimitBps   = globalLimitBps;
        this.patienceSeconds  = Math.max(0, Math.min(MAX_PATIENCE_S, patienceSeconds));   /* the core refuses to start on a value outside its range */
    }

    /* ── Startup ──────────────────────────────────────────────────────────── */

    @PostConstruct
    public void startup() {
        try {
            ensureConnected(startupTimeoutMs);
        } catch (IOException e) {
            log.error("could not start fluxdm-core: {}", e.getMessage());
            log.error("check fluxdm.core.binary-path in application.properties (now {}) and that "
                      + "'make' has been run in core/", coreBinaryPath);
            throw new IllegalStateException("Could not start fluxdm-core", e);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("Interrupted while starting fluxdm-core", e);
        }
        connectedSinceMs = System.currentTimeMillis();

        supervisor = Executors.newSingleThreadScheduledExecutor(r -> {
            Thread t = new Thread(r, "fluxdm-core-supervisor");
            t.setDaemon(true);
            return t;
        });
        supervisor.scheduleWithFixedDelay(this::superviseOnce, 1000, 1000, TimeUnit.MILLISECONDS);
    }

    /** Makes sure there is a core and a connection to it, within timeoutMs. */
    private void ensureConnected(long timeoutMs) throws IOException, InterruptedException {
        synchronized (connectLock) {
            if (ipc.isConnected()) return;

            Process mine = coreProcess;
            if (mine == null || !mine.isAlive()) {
                coreProcess = null;
                try {
                    ipc.connect();
                    log.info("connected to the fluxdm-core already running on {}", ipc.socketPath());
                    return;
                } catch (IOException nobodyThere) {
                    /* nothing is listening: start our own */
                }
                spawnCore();
            }

            long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
            IOException last = null;
            while (!stopping) {
                try {
                    ipc.connect();
                    log.info("connected to fluxdm-core on {}", ipc.socketPath());
                    return;
                } catch (IOException e) {
                    last = e;
                }
                Process cur = coreProcess;
                if (cur != null && !cur.isAlive()) {
                    coreProcess = null;
                    throw new IOException("fluxdm-core exited immediately (exit code " + cur.exitValue()
                                          + ") — its own message is in the log above");
                }
                if (System.nanoTime() >= deadline) break;
                Thread.sleep(50);
            }
            if (stopping) throw new IOException("the daemon is stopping");

            killCore();                                   /* alive but never listened: do not leave it around */
            throw new IOException("fluxdm-core did not start listening on " + ipc.socketPath()
                                  + " within " + timeoutMs + " ms" + (last != null ? " (" + last.getMessage() + ")" : ""));
        }
    }

    private void spawnCore() throws IOException {
        Path binary = Path.of(coreBinaryPath);
        if (!Files.isRegularFile(binary)) {
            throw new IOException("fluxdm-core binary not found at " + coreBinaryPath
                                  + " — build it first: cd core && make");
        }
        if (!Files.isExecutable(binary)) {
            throw new IOException("fluxdm-core at " + coreBinaryPath + " is not executable");
        }

        List<String> cmd = new ArrayList<>();
        cmd.add(binary.toAbsolutePath().toString());
        cmd.add("--sock");
        cmd.add(ipc.socketPath());
        cmd.add("--log");
        cmd.add(String.valueOf(logLevel));
        if (globalLimitBps > 0) {
            cmd.add("--limit");
            cmd.add(String.valueOf(globalLimitBps));
        }
        cmd.add("--patience");
        cmd.add(String.valueOf(patienceSeconds));

        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
        /* stderr stays visible in the daemon's own log stream: the core's banner and
         * error messages are genuinely useful. */
        pb.redirectError(ProcessBuilder.Redirect.INHERIT);

        coreProcess = pb.start();
        log.info("spawned fluxdm-core (pid {})", coreProcess.pid());
    }

    private void killCore() {
        Process p = coreProcess;
        coreProcess = null;
        if (p != null && p.isAlive()) p.destroyForcibly();
    }

    /* ── Supervision ──────────────────────────────────────────────────────── */

    private void superviseOnce() {
        if (stopping) return;
        try {
            long now = System.currentTimeMillis();
            if (ipc.isConnected()) {
                if (now - connectedSinceMs >= STABLE_MS) retryDelayMs = FIRST_RETRY_MS;
                return;
            }
            if (now < nextAttemptMs) return;

            Process p = coreProcess;
            if (p != null && !p.isAlive()) {
                log.warn("fluxdm-core exited (exit code {}); restarting it", p.exitValue());
                coreProcess = null;
                restarts.incrementAndGet();
            }
            try {
                ensureConnected(5_000);
                connectedSinceMs = System.currentTimeMillis();
            } catch (IOException e) {
                log.error("could not (re)connect to fluxdm-core: {} — next attempt in {} ms", e.getMessage(), retryDelayMs);
                nextAttemptMs = now + retryDelayMs;
                retryDelayMs  = Math.min(retryDelayMs * 2, MAX_RETRY_MS);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException e) {
            log.error("fluxdm-core supervisor failed", e);
        }
    }

    /* ── Shutdown ─────────────────────────────────────────────────────────── */

    @PreDestroy
    public void shutdown() {
        stopping = true;
        if (supervisor != null) {
            supervisor.shutdownNow();
            try { supervisor.awaitTermination(2, TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }

        Process p = coreProcess;
        if (p != null && p.isAlive()) {
            log.info("shutting down fluxdm-core...");
            if (ipc.isConnected()) {
                try {
                    /* The core flushes its resume checkpoints before it answers and exits. */
                    ipc.shutdownCore().get(2, TimeUnit.SECONDS);
                } catch (Exception ignored) {
                    /* best effort — it is being stopped regardless */
                }
            }
            ipc.disconnect();
            try {
                if (!p.waitFor(3, TimeUnit.SECONDS)) {
                    p.destroy();                          /* SIGTERM — the core handles it gracefully too */
                    if (!p.waitFor(3, TimeUnit.SECONDS)) {
                        log.warn("fluxdm-core did not exit in time, forcing");
                        p.destroyForcibly();
                    }
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                p.destroyForcibly();
            }
            log.info("fluxdm-core stopped");
        } else {
            ipc.disconnect();                             /* an adopted core is left running */
        }
        coreProcess = null;
    }

    /* ── Introspection ────────────────────────────────────────────────────── */

    public boolean isCoreAlive() {
        return ipc.isConnected();
    }

    public Map<String, Object> status() {
        Process p = coreProcess;
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("connected", ipc.isConnected());
        m.put("socket",    ipc.socketPath());
        m.put("spawnedByDaemon", p != null);
        m.put("pid",       p != null && p.isAlive() ? p.pid() : null);
        m.put("restarts",  restarts.get());
        return m;
    }
}
