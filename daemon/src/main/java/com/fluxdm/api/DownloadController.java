/* FluxDM — api/DownloadController.java */
package com.fluxdm.api;
import com.fluxdm.model.Download;
import com.fluxdm.service.DownloadService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;
import java.util.List;

@RestController
@RequestMapping("/api/downloads")
public class DownloadController {

    @Autowired private DownloadService downloadService;

    @GetMapping
    public List<Download> getAll() { return downloadService.getAll(); }

    @GetMapping("/{id}")
    public ResponseEntity<Download> getOne(@PathVariable long id) {
        return downloadService.findById(id).map(ResponseEntity::ok)
               .orElse(ResponseEntity.notFound().build());
    }

    @PostMapping
    public Download create(@RequestBody Download dl) { return downloadService.start(dl); }

    @PostMapping("/{id}/pause")
    public void pause(@PathVariable long id) { downloadService.pause(id); }

    @PostMapping("/{id}/resume")
    public void resume(@PathVariable long id) { downloadService.resume(id); }

    @DeleteMapping("/{id}")
    public void cancel(@PathVariable long id) { downloadService.cancel(id); }
}
