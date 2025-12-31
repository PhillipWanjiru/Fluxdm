/* FluxDM — model/ScheduleTask.java */
package com.fluxdm.model;
import jakarta.persistence.*;

@Entity
@Table(name = "schedule_tasks")
public class ScheduleTask {
    @Id @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long    id;
    private Long    downloadId;
    private Long    triggerAt;
    private Long    repeatSecs = 0L;
    private Boolean enabled    = true;

    public Long    getId()          { return id; }
    public Long    getDownloadId()  { return downloadId; }
    public void    setDownloadId(Long d){ this.downloadId = d; }
    public Long    getTriggerAt()   { return triggerAt; }
    public void    setTriggerAt(Long t){ this.triggerAt = t; }
    public Long    getRepeatSecs()  { return repeatSecs; }
    public void    setRepeatSecs(Long r){ this.repeatSecs = r; }
    public Boolean getEnabled()     { return enabled; }
    public void    setEnabled(Boolean e){ this.enabled = e; }
}
