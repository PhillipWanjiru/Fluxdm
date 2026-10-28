/* FluxDM — service/SchedulerService.java */
package com.fluxdm.service;
import com.fluxdm.db.ScheduleRepository;
import com.fluxdm.model.ScheduleTask;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;
import java.time.Instant;
import java.util.List;

@Service
public class SchedulerService {

    private static final Logger log = LoggerFactory.getLogger(SchedulerService.class);

    private final ScheduleRepository repository;
    private final DownloadService    downloadService;

    public SchedulerService(ScheduleRepository repository, DownloadService downloadService) {
        this.repository      = repository;
        this.downloadService = downloadService;
    }

    public List<ScheduleTask> getAll()              { return repository.findAll(); }
    public ScheduleTask       create(ScheduleTask t){ return repository.save(t); }
    public void               delete(long id)       { repository.deleteById(id); }

    @Scheduled(fixedDelay = 10_000)
    public void tick() {
        long now = Instant.now().getEpochSecond();
        for (ScheduleTask task : repository.findByEnabledTrueAndTriggerAtLessThanEqual(now)) {
            try {
                if (task.getDownloadId() != null) downloadService.resume(task.getDownloadId());
            } catch (DownloadException e) {
                if (e.kind() == DownloadException.Kind.UNAVAILABLE) {
                    /* fluxdm-core is not ready: leave the task as it is and try again next tick */
                    log.warn("scheduled resume of download {} postponed: {}", task.getDownloadId(), e.getMessage());
                    continue;
                }
                /* the download is gone, or already complete: nothing to resume, the schedule still counts as fired */
                log.warn("scheduled resume of download {} skipped: {}", task.getDownloadId(), e.getMessage());
            } catch (RuntimeException e) {
                log.error("scheduled resume of download {} failed", task.getDownloadId(), e);
                continue;
            }

            if (task.getRepeatSecs() != null && task.getRepeatSecs() > 0) {
                task.setTriggerAt(now + task.getRepeatSecs());
            } else {
                task.setEnabled(false);
            }
            repository.save(task);
        }
    }
}
