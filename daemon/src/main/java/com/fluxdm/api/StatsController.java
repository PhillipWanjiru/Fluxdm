/* FluxDM — api/StatsController.java */
package com.fluxdm.api;
import com.fluxdm.model.Stats;
import com.fluxdm.service.DownloadService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;

@RestController
@RequestMapping("/api/stats")
public class StatsController {
    @Autowired private DownloadService downloadService;

    @GetMapping
    public Stats getStats() { return downloadService.getStats(); }
}
