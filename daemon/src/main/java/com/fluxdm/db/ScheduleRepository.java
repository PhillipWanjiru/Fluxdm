/* FluxDM — db/ScheduleRepository.java */
package com.fluxdm.db;
import com.fluxdm.model.ScheduleTask;
import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.stereotype.Repository;
import java.util.List;

@Repository
public interface ScheduleRepository extends JpaRepository<ScheduleTask, Long> {
    List<ScheduleTask> findByEnabledTrueAndTriggerAtLessThanEqual(long now);
}
