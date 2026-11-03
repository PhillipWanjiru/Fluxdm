/* FluxDM — model/tasks.js
 * A scheduled task: at `triggerAt` the daemon resumes download `downloadId`; with `repeatSecs` above 0 it does
 * so again that many seconds later, otherwise the task switches itself off (service/SchedulerService, which
 * looks every 10 seconds, so a task fires up to 10 seconds late). Times are epoch seconds. */

const whole = (v) => {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
};

export function taskFromDaemon(json) {
  const t = json || {};
  return {
    id: Number(t.id),
    downloadId: t.downloadId == null ? null : Number(t.downloadId),
    triggerAt: whole(t.triggerAt),
    repeatSecs: whole(t.repeatSecs),
    enabled: t.enabled !== false,
  };
}

/* The body of POST /api/scheduler for a new task. */
export function taskToDaemon({ downloadId, triggerAt, repeatSecs = 0, enabled = true }) {
  return { downloadId, triggerAt, repeatSecs, enabled };
}
