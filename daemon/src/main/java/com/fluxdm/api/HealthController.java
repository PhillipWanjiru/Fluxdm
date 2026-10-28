/* FluxDM — api/HealthController.java
 * GET /api/health — is the daemon up, and is fluxdm-core connected and in step? */
package com.fluxdm.api;
import com.fluxdm.service.DownloadService;
import com.fluxdm.service.IPCBridgeService;
import org.springframework.web.bind.annotation.*;
import java.util.LinkedHashMap;
import java.util.Map;

@RestController
@RequestMapping("/api/health")
public class HealthController {

    private final IPCBridgeService bridge;
    private final DownloadService  downloads;

    public HealthController(IPCBridgeService bridge, DownloadService downloads) {
        this.bridge    = bridge;
        this.downloads = downloads;
    }

    @GetMapping
    public Map<String, Object> health() {
        boolean ok = bridge.isCoreAlive() && downloads.isSynced();
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("status", ok ? "ok" : "degraded");
        m.put("synced", downloads.isSynced());
        m.put("core",   bridge.status());
        return m;
    }
}
