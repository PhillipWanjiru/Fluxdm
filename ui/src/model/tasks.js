/* FluxDM — model/tasks.js
 * A scheduled task: at `triggerAt` the daemon resumes download `downloadId`; with `repeatSecs` above 0 it does
 * so again that many seconds after each run, otherwise the task switches itself off (service/SchedulerService,
 * which looks every 10 seconds, so a task fires up to 10 seconds late). Times are epoch seconds.
 *
 * The daemon checks nothing when a task is created (api/SchedulerController takes the body as it is), so what
 * makes a sensible task is decided here: validateTask(). */
import { State } from "./downloads";

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

/* ── What a task is for ───────────────────────────────────────────────────────────────────────────────────── */

/* The downloads worth scheduling: a paused one is resumed, a failed one is tried again. (A running download has
 * nothing to wait for, a finished one nothing to resume, and what resuming a queued one does is up to the
 * daemon's queue.) */
export const SCHEDULABLE_STATES = Object.freeze([State.PAUSED, State.FAILED]);

/* The repeats the form offers. The daemon takes any number of seconds, and a task made elsewhere may have
 * another one, which describeRepeat() still reads. */
export const REPEAT_CHOICES = Object.freeze([
  { secs: 0, label: "Once" },
  { secs: 86400, label: "Every day" },
  { secs: 604800, label: "Every week" },
]);

const REPEAT_UNITS = [
  [604800, "week"],
  [86400, "day"],
  [3600, "hour"],
  [60, "minute"],
  [1, "second"],
];

/* 0 -> "Once", 86400 -> "Every day", 7200 -> "Every 2 hours", 90 -> "Every 90 seconds". */
export function describeRepeat(secs) {
  const total = Math.floor(Number(secs));
  if (!(total > 0)) return "Once";
  const [size, name] = REPEAT_UNITS.find(([unit]) => total % unit === 0);
  const n = total / size;
  return n === 1 ? `Every ${name}` : `Every ${n} ${name}s`;
}

/* Where a task stands: waiting for its time, due (its time has come and the daemon has not looked yet, which
 * lasts a few seconds), or finished (it ran and, not repeating, switched itself off). */
export const Phase = Object.freeze({ WAITING: "waiting", DUE: "due", FINISHED: "finished" });

export function phaseOf(task, nowSecs) {
  if (!task.enabled) return Phase.FINISHED;
  return task.triggerAt > nowSecs ? Phase.WAITING : Phase.DUE;
}

/* Tasks that will still run first, soonest first; then the finished ones, the latest first. */
export function sortTasks(tasks) {
  return [...tasks].sort((a, b) => {
    if (a.enabled !== b.enabled) return a.enabled ? -1 : 1;
    const byTime = a.enabled ? a.triggerAt - b.triggerAt : b.triggerAt - a.triggerAt;
    return byTime || a.id - b.id;
  });
}

/* ── Times ────────────────────────────────────────────────────────────────────────────────────────────────── */

const LOCAL_TIME = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})(?::(\d{2}))?$/;

/* The text of a datetime-local input ("2026-11-06T02:00", in the user's own time zone) as epoch seconds; null
 * when it is empty, malformed, or not a real time (30 February, or the hour that is skipped when the clocks go
 * forward). */
export function parseLocalTime(text) {
  const match = LOCAL_TIME.exec(text || "");
  if (!match) return null;
  const [year, month, day, hour, minute, second] = match.slice(1).map((part) => Number(part ?? 0)); /* seconds are optional */
  const date = new Date(year, month - 1, day, hour, minute, second);
  const real =
    date.getFullYear() === year && date.getMonth() === month - 1 && date.getDate() === day &&
    date.getHours() === hour && date.getMinutes() === minute && date.getSeconds() === second;
  return real ? Math.floor(date.getTime() / 1000) : null;
}

/* A moment (epoch milliseconds) as the text of a datetime-local input, to the minute. */
export function toLocalInput(ms) {
  const d = new Date(ms);
  const pad = (n, width = 2) => String(n).padStart(width, "0");
  return `${pad(d.getFullYear(), 4)}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/* A task's time as the list shows it, in the user's language and time zone. */
export const formatWhen = (epochSecs) =>
  new Date(epochSecs * 1000).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });

/* ── Checking a new task ──────────────────────────────────────────────────────────────────────────────────── */

/* The first thing wrong with a task about to be created, as { field: "download" | "when", message }, or null.
 * `when` is the text of the datetime-local input, `nowMs` the time to measure "in the future" against. */
export function validateTask({ downloadId, when, nowMs }) {
  if (!Number.isInteger(downloadId)) return { field: "download", message: "Choose a download." };
  const triggerAt = parseLocalTime(when);
  if (triggerAt === null) {
    return { field: "when", message: when ? "That is not a valid date and time." : "Choose when to start it." };
  }
  if (triggerAt * 1000 <= nowMs) return { field: "when", message: "Choose a time in the future." };
  return null;
}
