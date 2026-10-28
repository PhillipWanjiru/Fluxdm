/* FluxDM — service/BandwidthService.java
 * The daemon-wide speed limit. The core forgets it when it restarts, so the
 * value lives here and is applied again after every (re)connection.
 */
package com.fluxdm.service;

import com.fluxdm.ipc.CoreIPCClient;
import com.fluxdm.ipc.CoreIPCClient.AckResult;
import com.fluxdm.ipc.IPCEventListener;

import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

@Service
public class BandwidthService implements IPCEventListener {

    private static final Logger log = LoggerFactory.getLogger(BandwidthService.class);

    private final CoreIPCClient ipc;
    private volatile long globalLimitBps;

    public BandwidthService(CoreIPCClient ipc,
                            @Value("${fluxdm.core.global-limit-bps:0}") long initialLimitBps) {
        this.ipc = ipc;
        this.globalLimitBps = Math.max(0L, initialLimitBps);
    }

    @PostConstruct
    void registerAsListener() {
        ipc.addListener(this);
    }

    /** Bytes per second across all downloads, 0 = unlimited. */
    public long getGlobalLimit() { return globalLimitBps; }

    /** Sets the daemon-wide limit. If the core cannot be reached the new value is
     *  still remembered and takes effect as soon as it is back. */
    public void setGlobalLimit(long bps) {
        if (bps < 0) throw DownloadException.badRequest("the limit cannot be negative");
        this.globalLimitBps = bps;

        AckResult res;
        try {
            res = ipc.setLimit(0, bps).get(CoreIPCClient.ACK_TIMEOUT_SECS + 5L, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw DownloadException.unavailable("interrupted while waiting for fluxdm-core");
        } catch (ExecutionException | TimeoutException e) {
            throw DownloadException.unavailable("fluxdm-core did not answer");
        }
        if (!res.ok() && !res.coreUnreachable())
            throw DownloadException.unavailable(res.errorMessage() != null ? res.errorMessage() : "fluxdm-core refused the limit");
    }

    @Override
    public void onConnected() {
        /* Always, even for 0: a core the supervisor restarted starts from the configured
         * limit, which may no longer be what the user chose. */
        ipc.setLimit(0, globalLimitBps).whenComplete((res, ex) -> {
            if (ex != null || !res.ok())
                log.warn("could not apply the global bandwidth limit to fluxdm-core: {}",
                         ex != null ? ex : res.errorMessage());
        });
    }
}
