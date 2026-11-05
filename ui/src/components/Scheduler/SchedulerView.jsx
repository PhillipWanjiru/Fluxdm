/* FluxDM — Scheduler/SchedulerView.jsx
 * The daemon's scheduled tasks: make one, see what is waiting and what has run, delete one.
 *
 * The daemon looks at its tasks every ten seconds, and a task that has run switches itself off or moves on to its
 * next time, so the list is read again at that rhythm while this view is open. The names come from the list of
 * downloads the live feed keeps in the store; a task whose download was removed says so and can be deleted. */
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { useSelector } from "react-redux";
import { createTask, deleteTask, getTasks } from "../../api/scheduler";
import ConfirmModal from "../Modals/ConfirmModal";
import ScheduleForm from "./ScheduleForm";
import TaskCard from "./TaskCard";
import { SCHEDULABLE_STATES, sortTasks } from "../../model/tasks";
import { selectAllDownloads } from "../../store/downloadsSlice";

export const REFRESH_MS = 10000;

const alertBox = { marginBottom: 12, padding: "8px 10px", background: "rgba(244,74,74,0.08)", border: "1px solid var(--red)", borderRadius: 6, color: "var(--red)", fontSize: 11, wordBreak: "break-word" };

export default function SchedulerView() {
  const downloads = useSelector(selectAllDownloads);
  const [tasks, setTasks] = useState(null); /* null until the first answer */
  const [loadError, setLoadError] = useState("");
  const [problem, setProblem] = useState(""); /* why the last delete failed */
  const [asking, setAsking] = useState(null); /* the task whose deletion is being confirmed */
  const [deleting, setDeleting] = useState(null); /* the id of the task being deleted */
  const [nowMs, setNowMs] = useState(() => Date.now());

  /* Reads are numbered, and only the newest one counts: an answer that arrives after a newer read was started, after
   * a task was made or deleted here, or after this view is gone, is older than what is shown and is dropped. */
  const newest = useRef(0);
  const read = useCallback(() => {
    const mine = ++newest.current;
    getTasks().then(
      (list) => {
        if (mine !== newest.current) return;
        setTasks(sortTasks(list));
        setLoadError("");
        setNowMs(Date.now());
      },
      (error) => {
        if (mine === newest.current) setLoadError(error.message);
      }
    );
  }, []);

  useEffect(() => {
    read();
    const timer = setInterval(read, REFRESH_MS);
    return () => {
      clearInterval(timer);
      newest.current += 1;
    };
  }, [read]);

  const byId = useMemo(() => new Map(downloads.map((d) => [d.id, d])), [downloads]);
  const candidates = useMemo(() => downloads.filter((d) => SCHEDULABLE_STATES.includes(d.state)), [downloads]);

  const create = async (task) => {
    const created = await createTask(task);
    newest.current += 1;
    setTasks((list) => sortTasks([...(list || []).filter((t) => t.id !== created.id), created]));
    setNowMs(Date.now());
  };

  const confirmDelete = async () => {
    const task = asking;
    setAsking(null);
    setDeleting(task.id);
    setProblem("");
    const forget = () => {
      newest.current += 1;
      setTasks((list) => (list || []).filter((t) => t.id !== task.id));
    };
    try {
      await deleteTask(task.id);
      forget();
    } catch (error) {
      if (error.kind === "not_found") forget(); /* it is gone, as asked */
      else setProblem(error.message);
    } finally {
      setDeleting(null);
    }
  };

  const nowSecs = Math.floor(nowMs / 1000);
  const nameOf = (task) => (byId.get(task.downloadId) || { name: `Download #${task.downloadId}` }).name;

  return (
    <div style={{ padding: 24, height: "100%", overflowY: "auto" }}>
      <h2 style={{ fontSize: 14, fontWeight: 700, letterSpacing: 1, marginBottom: 20, color: "var(--green)" }}>SCHEDULER</h2>

      <ScheduleForm candidates={candidates} onCreate={create} />

      {problem && <div role="alert" style={alertBox}>{problem}</div>}
      {loadError && (
        <div role="alert" style={{ ...alertBox, color: "var(--amber)", borderColor: "var(--amber)", background: "rgba(244,196,74,0.06)" }}>
          {tasks === null ? "Could not load the schedule" : "Could not refresh the schedule"}: {loadError}
        </div>
      )}

      {tasks === null && !loadError && <div style={{ color: "var(--text-dim)", fontSize: 11 }}>Loading…</div>}
      {tasks !== null && tasks.length === 0 && (
        <div style={{ color: "var(--text-dim)", fontSize: 11 }}>No scheduled tasks.</div>
      )}
      {tasks !== null && tasks.length > 0 && (
        <ul aria-label="Scheduled tasks" style={{ padding: 0 }}>
          {tasks.map((task) => (
            <TaskCard
              key={task.id}
              task={task}
              download={byId.get(task.downloadId) || null}
              nowSecs={nowSecs}
              busy={deleting === task.id}
              onDelete={() => setAsking(task)}
            />
          ))}
        </ul>
      )}

      {asking && (
        <ConfirmModal
          title="Delete this schedule?"
          message={`${nameOf(asking)} will no longer be started by it. The download itself is not touched.`}
          confirmLabel="Delete"
          onCancel={() => setAsking(null)}
          onConfirm={confirmDelete}
        />
      )}
    </div>
  );
}
