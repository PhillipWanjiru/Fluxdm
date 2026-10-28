/* FluxDM — api/BandwidthController.java
 * The daemon-wide speed limit. GET returns {"globalLimitBps": n};
 * PUT takes the same shape. 0 = unlimited. */
package com.fluxdm.api;
import com.fluxdm.service.BandwidthService;
import com.fluxdm.service.DownloadException;
import org.springframework.web.bind.annotation.*;
import java.util.Map;

@RestController
@RequestMapping("/api/bandwidth")
public class BandwidthController {

    private final BandwidthService bandwidthService;

    public BandwidthController(BandwidthService bandwidthService) {
        this.bandwidthService = bandwidthService;
    }

    @GetMapping
    public Map<String, Long> get() {
        return Map.of("globalLimitBps", bandwidthService.getGlobalLimit());
    }

    @PutMapping
    public Map<String, Long> set(@RequestBody Map<String, Long> body) {
        Long v = body.get("globalLimitBps");
        if (v == null) throw DownloadException.badRequest("globalLimitBps is required");
        bandwidthService.setGlobalLimit(v);
        return Map.of("globalLimitBps", bandwidthService.getGlobalLimit());
    }
}
