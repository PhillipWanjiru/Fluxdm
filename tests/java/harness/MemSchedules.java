/* FluxDM — tests/java/harness/MemSchedules.java */
package harness;

import com.fluxdm.db.ScheduleRepository;
import com.fluxdm.model.ScheduleTask;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;

public final class MemSchedules implements ScheduleRepository {

    public final List<ScheduleTask> tasks = new CopyOnWriteArrayList<>();

    @Override public List<ScheduleTask> findAll() { return new ArrayList<>(tasks); }
    @Override public Optional<ScheduleTask> findById(Long id) { return Optional.empty(); }
    @Override public <S extends ScheduleTask> S save(S e) { if (!tasks.contains(e)) tasks.add(e); return e; }
    @Override public void deleteById(Long id) { }
    @Override public long count() { return tasks.size(); }
    @Override public boolean existsById(Long id) { return false; }

    @Override public List<ScheduleTask> findByEnabledTrueAndTriggerAtLessThanEqual(long now) {
        List<ScheduleTask> out = new ArrayList<>();
        for (ScheduleTask t : tasks)
            if (Boolean.TRUE.equals(t.getEnabled()) && t.getTriggerAt() != null && t.getTriggerAt() <= now) out.add(t);
        return out;
    }
}
