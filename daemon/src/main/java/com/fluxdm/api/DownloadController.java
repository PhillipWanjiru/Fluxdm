/* FluxDM — api/DownloadController.java */
package com.fluxdm.api;
import com.fluxdm.model.Download;
import com.fluxdm.service.DownloadException;
import com.fluxdm.service.DownloadService;
import org.springframework.web.bind.annotation.*;
import java.util.List;
import java.util.Map;

@RestController
@RequestMapping("/api/downloads")
public class DownloadController {

    private final DownloadService downloadService;

    public DownloadController(DownloadService downloadService) {
        this.downloadService = downloadService;
    }

    @GetMapping
    public List<Download> getAll() { return downloadService.getAll(); }

    @GetMapping("/{id}")
    public Download getOne(@PathVariable long id) {
        return downloadService.findById(id).orElseThrow(() -> DownloadException.notFound(id));
    }

    @PostMapping
    public Download create(@RequestBody Download dl) { return downloadService.start(dl); }

    @PostMapping("/{id}/pause")
    public Download pause(@PathVariable long id) { return downloadService.pause(id); }

    @PostMapping("/{id}/resume")
    public Download resume(@PathVariable long id) { return downloadService.resume(id); }

    /** Body: {"bytesPerSec": 500000} — 0 removes the limit. */
    @PutMapping("/{id}/limit")
    public Download limit(@PathVariable long id, @RequestBody Map<String, Long> body) {
        Long v = body.get("bytesPerSec");
        if (v == null) throw DownloadException.badRequest("bytesPerSec is required");
        return downloadService.setLimit(id, v);
    }

    @DeleteMapping("/{id}")
    public void cancel(@PathVariable long id) { downloadService.cancel(id); }
}
