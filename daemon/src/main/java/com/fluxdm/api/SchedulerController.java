/* FluxDM — api/SchedulerController.java */
package com.fluxdm.api;
import com.fluxdm.model.ScheduleTask;
import com.fluxdm.service.SchedulerService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;
import java.util.List;

@RestController
@RequestMapping("/api/scheduler")
public class SchedulerController {
    @Autowired private SchedulerService schedulerService;

    @GetMapping
    public List<ScheduleTask> getAll() { return schedulerService.getAll(); }

    @PostMapping
    public ScheduleTask create(@RequestBody ScheduleTask task) { return schedulerService.create(task); }

    @DeleteMapping("/{id}")
    public void delete(@PathVariable long id) { schedulerService.delete(id); }
}
