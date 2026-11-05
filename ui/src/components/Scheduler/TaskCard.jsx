/* FluxDM — Scheduler/TaskCard.jsx
 * One scheduled task: which download it starts, when, how often, and whether it is still to run. `download` is the
 * download it names as the list knows it, or null when the list does not have it (it was removed). */
import React from "react";
import { Phase, describeRepeat, formatWhen, phaseOf } from "../../model/tasks";
import { formatEta } from "../../model/format";

const BADGE = {
  [Phase.WAITING]: { label: "WAITING", color: "#4af48a", bg: "#0a1f12" },
  [Phase.DUE]: { label: "DUE", color: "#f4c44a", bg: "#1f1a0a" },
  [Phase.FINISHED]: { label: "FINISHED", color: "#888", bg: "#1a1a1a" },
};

function timeLine(task, phase, nowSecs) {
  const when = formatWhen(task.triggerAt);
  if (phase === Phase.WAITING) return `Starts ${when} (in ${formatEta(task.triggerAt - nowSecs)})`;
  if (phase === Phase.DUE) return `Due ${when}: starting now`;
  return `Ran ${when}`;
}

export default function TaskCard({ task, download, nowSecs, busy = false, onDelete }) {
  const phase = phaseOf(task, nowSecs);
  const badge = BADGE[phase];
  const name = download ? download.name : `Download #${task.downloadId}`;
  const repeats = task.repeatSecs > 0;

  return (
    <li
      data-testid={`task-${task.id}`}
      style={{
        listStyle: "none",
        background: "var(--bg-panel)",
        border: "1px solid var(--border)",
        borderRadius: 8,
        padding: "12px 16px",
        marginBottom: 8,
        display: "flex",
        justifyContent: "space-between",
        alignItems: "center",
        gap: 16,
        opacity: phase === Phase.FINISHED ? 0.7 : 1,
      }}
    >
      <div style={{ minWidth: 0 }}>
        <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 4 }}>
          <span title={name} style={{ fontSize: 12, color: "var(--text-main)", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
            {name}
          </span>
          <span
            style={{
              fontSize: 9, fontWeight: 700, letterSpacing: 1, padding: "3px 7px", borderRadius: 4, flexShrink: 0,
              color: badge.color, background: badge.bg, border: `1px solid ${badge.color}33`, fontFamily: "monospace",
            }}
          >
            {badge.label}
          </span>
        </div>
        <div style={{ fontSize: 10, color: "var(--text-dim)", wordBreak: "break-all" }}>
          {download ? download.filePath : "No longer in the list of downloads"}
        </div>
        <div style={{ fontSize: 10, color: "#888", marginTop: 4 }}>
          {timeLine(task, phase, nowSecs)}
          {repeats ? ` · ${describeRepeat(task.repeatSecs)}` : ""}
        </div>
      </div>
      <button
        type="button"
        aria-label={`Delete schedule for ${name}`}
        disabled={busy}
        onClick={onDelete}
        style={{
          background: "none", border: "1px solid var(--red)", borderRadius: 4, color: "var(--red)", padding: "5px 10px",
          cursor: busy ? "default" : "pointer", opacity: busy ? 0.5 : 1, fontFamily: "monospace", fontSize: 10, flexShrink: 0,
        }}
      >
        Delete
      </button>
    </li>
  );
}
