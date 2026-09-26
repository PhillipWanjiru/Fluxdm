/* FluxDM — service/IPCBridgeService.java
 * Owns the fluxdm-core process lifecycle: spawns it on daemon startup,
 * waits for its Unix socket to appear, connects CoreIPCClient, and
 * shuts it down cleanly (CMD_SHUTDOWN, then a graceful process exit
 * with a forced-kill fallback) when the Spring context closes.
 */
package com.fluxdm.service;

import com.fluxdm.ipc.CoreIPCClient;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

@Service
public class IPCBridgeService {

    @Autowired private CoreIPCClient ipcClient;

    @Value("${fluxdm.core.binary-path}")
    private String coreBinaryPath;

    @Value("${fluxdm.core.socket-path:/tmp/fluxdm.sock}")
    private String socketPath;

    @Value("${fluxdm.core.log-level:1}")
    private int logLevel;

    @Value("${fluxdm.core.startup-timeout-ms:5000}")
    private long startupTimeoutMs;

    @Value("${fluxdm.core.global-limit-bps:0}")
    private long globalLimitBps;

    private Process coreProcess;

    /* ── Startup ──────────────────────────────────────────────────────────── */

    @PostConstruct
    public void startup() {
        try {
            spawnCore();
            waitForSocket();
            ipcClient.connect();
            System.out.println("[fluxdm-daemon] connected to fluxdm-core at " + socketPath);
        } catch (Exception e) {
            System.err.println("[fluxdm-daemon] FAILED to start fluxdm-core: " + e.getMessage());
            System.err.println("[fluxdm-daemon] check fluxdm.core.binary-path in application.properties: "
                               + coreBinaryPath);
            throw new IllegalStateException("Could not start fluxdm-core", e);
        }
    }

    private void spawnCore() throws IOException {
        Path binary = Path.of(coreBinaryPath);
        if (!Files.isRegularFile(binary)) {
            throw new IOException("fluxdm-core binary not found at " + coreBinaryPath +
                " — build it first: cd core && make");
        }
        if (!Files.isExecutable(binary)) {
            throw new IOException("fluxdm-core at " + coreBinaryPath + " is not executable");
        }

        /* Remove a stale socket left over from a previous unclean shutdown,
         * otherwise fluxdm-core's bind() will fail. */
        Path sock = Path.of(socketPath);
        Files.deleteIfExists(sock);

        List<String> cmd = new ArrayList<>();
        cmd.add(binary.toAbsolutePath().toString());
        cmd.add("--log");
        cmd.add(String.valueOf(logLevel));
        if (globalLimitBps > 0) {
            cmd.add("--limit");
            cmd.add(String.valueOf(globalLimitBps));
        }

        ProcessBuilder pb = new ProcessBuilder(cmd);
        pb.redirectErrorStream(false);
        pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
        /* Keep stderr visible in the daemon's own log stream — fluxdm-core's
         * banner and error messages are genuinely useful during development. */
        pb.redirectError(ProcessBuilder.Redirect.INHERIT);

        coreProcess = pb.start();
        System.out.println("[fluxdm-daemon] spawned fluxdm-core (pid " + coreProcess.pid() + ")");
    }

    private void waitForSocket() throws IOException, InterruptedException {
        Path sock = Path.of(socketPath);
        long deadline = System.currentTimeMillis() + startupTimeoutMs;

        while (System.currentTimeMillis() < deadline) {
            if (Files.exists(sock)) return;
            if (!coreProcess.isAlive()) {
                throw new IOException("fluxdm-core exited immediately (exit code "
                    + coreProcess.exitValue() + ") — check stderr above");
            }
            Thread.sleep(50);
        }
        throw new IOException("Timed out waiting for " + socketPath +
            " to appear after " + startupTimeoutMs + "ms");
    }

    /* ── Shutdown ─────────────────────────────────────────────────────────── */

    @PreDestroy
    public void shutdown() {
        System.out.println("[fluxdm-daemon] shutting down fluxdm-core...");

        if (ipcClient.isConnected()) {
            try {
                ipcClient.shutdownCore();
                /* Give the core a moment to flush resume state and exit cleanly
                 * before we tear down the connection out from under it. */
                Thread.sleep(300);
            } catch (Exception ignored) {
                /* Best-effort — we're shutting down regardless */
            }
        }
        ipcClient.disconnect();

        if (coreProcess != null && coreProcess.isAlive()) {
            coreProcess.destroy();                 /* SIGTERM — core handles this gracefully */
            try {
                boolean exited = coreProcess.waitFor(3, java.util.concurrent.TimeUnit.SECONDS);
                if (!exited) {
                    System.err.println("[fluxdm-daemon] fluxdm-core did not exit in time, forcing");
                    coreProcess.destroyForcibly();
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                coreProcess.destroyForcibly();
            }
        }
        System.out.println("[fluxdm-daemon] fluxdm-core stopped");
    }

    public boolean isCoreAlive() {
        return coreProcess != null && coreProcess.isAlive() && ipcClient.isConnected();
    }
}
