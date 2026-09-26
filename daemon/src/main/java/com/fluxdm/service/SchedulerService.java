/* FluxDM — service/SchedulerService.java */
package com.fluxdm.service;
import com.fluxdm.db.ScheduleRepository;
import com.fluxdm.model.ScheduleTask;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;
import java.time.Instant;
import java.util.List;

@Service
public class SchedulerService {

    @Autowired private ScheduleRepository repository;
    @Autowired private DownloadService    downloadService;

    public List<ScheduleTask> getAll()              { return repository.findAll(); }
    public ScheduleTask       create(ScheduleTask t){ return repository.save(t); }
    public void               delete(long id)       { repository.deleteById(id); }

    @Scheduled(fixedDelay = 10_000)
    public void tick() {
        long now = Instant.now().getEpochSecond();
        repository.findByEnabledTrueAndTriggerAtLessThanEqual(now).forEach(task -> {
            downloadService.resume(task.getDownloadId());
            if (task.getRepeatSecs() > 0) {
                task.setTriggerAt(now + task.getRepeatSecs());
                repository.save(task);
            } else {
                task.setEnabled(false);
                repository.save(task);
            }
        });
    }
}
