/* FluxDM — Scheduler/ScheduleForm.jsx
 * Makes a task: a download to start, when, and how often. Only downloads that resuming makes sense for are
 * offered (model/tasks.js, SCHEDULABLE_STATES). What is wrong with the input is said beside the field, and what the
 * daemon refuses is said above the buttons. */
import React, { useState } from "react";
import CalendarPicker from "./CalendarPicker";
import { REPEAT_CHOICES, parseLocalTime, toLocalInput, validateTask } from "../../model/tasks";

const label = { fontSize: 10, color: "var(--text-dim)", letterSpacing: 1, display: "block", marginBottom: 6 };
const field = (bad) => ({
  width: "100%", background: "#0a0a0a", border: `1px solid ${bad ? "var(--red)" : "var(--border)"}`, borderRadius: 6,
  padding: "9px 12px", color: "#fff", fontFamily: "monospace", fontSize: 12, outline: "none",
});
const hint = (bad) => ({ fontSize: 9, color: bad ? "var(--red)" : "var(--text-dim)", marginTop: 6, minHeight: 11 });

/* candidates: the downloads to choose from, as the list knows them. onCreate({ downloadId, triggerAt,
 * repeatSecs }) makes the task and rejects with an Error whose message is fit to show. */
export default function ScheduleForm({ candidates, onCreate, now = Date.now }) {
  const [downloadId, setDownloadId] = useState("");
  const [when, setWhen] = useState("");
  const [repeat, setRepeat] = useState("0");
  const [tried, setTried] = useState(false);
  const [busy, setBusy] = useState(false);
  const [refusal, setRefusal] = useState("");

  const problem = validateTask({ downloadId: downloadId === "" ? NaN : Number(downloadId), when, nowMs: now() });
  const shown = tried ? problem : null;

  const submit = async (event) => {
    event.preventDefault();
    setTried(true);
    if (busy) return;
    /* checked again at the moment of sending: the time that was in the future when typed may not be any more */
    const last = validateTask({ downloadId: downloadId === "" ? NaN : Number(downloadId), when, nowMs: now() });
    if (last) return;
    setBusy(true);
    setRefusal("");
    try {
      await onCreate({ downloadId: Number(downloadId), triggerAt: parseLocalTime(when), repeatSecs: Number(repeat) });
      setDownloadId("");
      setWhen("");
      setRepeat("0");
      setTried(false);
    } catch (error) {
      setRefusal(error.message);
    } finally {
      setBusy(false);
    }
  };

  const none = candidates.length === 0;

  return (
    <form
      onSubmit={submit}
      noValidate
      aria-labelledby="schedule-title"
      style={{ background: "var(--bg-panel)", border: "1px solid var(--border)", borderRadius: 8, padding: 16, marginBottom: 20 }}
    >
      <div id="schedule-title" style={{ fontSize: 10, color: "var(--text-dim)", letterSpacing: 1, marginBottom: 12 }}>
        NEW SCHEDULE
      </div>

      <div style={{ display: "grid", gridTemplateColumns: "2fr 1.4fr 1fr", gap: 12, alignItems: "start" }}>
        <div>
          <label htmlFor="schedule-download" style={label}>DOWNLOAD</label>
          <select
            id="schedule-download"
            value={downloadId}
            disabled={none || busy}
            onChange={(e) => { setDownloadId(e.target.value); setRefusal(""); }}
            aria-invalid={shown?.field === "download"}
            aria-describedby="schedule-download-help"
            style={field(shown?.field === "download")}
          >
            <option value="">{none ? "Nothing to schedule" : "Choose a download…"}</option>
            {candidates.map((d) => (
              <option key={d.id} value={d.id}>{`${d.name} (${d.state})`}</option>
            ))}
          </select>
          <div id="schedule-download-help" style={hint(shown?.field === "download")}>
            {shown?.field === "download"
              ? shown.message
              : none
                ? "Pause a download, or wait for one to fail: it can then be started at a time you choose."
                : "A paused download is resumed, a failed one is tried again."}
          </div>
        </div>

        <div>
          <label htmlFor="schedule-when" style={label}>START AT</label>
          <CalendarPicker
            id="schedule-when"
            value={when}
            onChange={(value) => { setWhen(value); setRefusal(""); }}
            min={toLocalInput(now())}
            invalid={shown?.field === "when"}
            describedBy="schedule-when-help"
          />
          <div id="schedule-when-help" style={hint(shown?.field === "when")}>
            {shown?.field === "when" ? shown.message : "Your local time."}
          </div>
        </div>

        <div>
          <label htmlFor="schedule-repeat" style={label}>REPEAT</label>
          <select id="schedule-repeat" value={repeat} disabled={busy} onChange={(e) => setRepeat(e.target.value)} style={field(false)}>
            {REPEAT_CHOICES.map(({ secs, label: text }) => (
              <option key={secs} value={secs}>{text}</option>
            ))}
          </select>
          <div style={hint(false)} />
        </div>
      </div>

      {refusal && (
        <div role="alert" style={{ margin: "4px 0 12px", padding: "8px 10px", background: "rgba(244,74,74,0.08)", border: "1px solid var(--red)", borderRadius: 6, color: "var(--red)", fontSize: 11, wordBreak: "break-word" }}>
          {refusal}
        </div>
      )}

      <button
        type="submit"
        disabled={busy || none}
        style={{ padding: "9px 18px", background: "rgba(74,244,138,0.1)", border: "1px solid var(--green)", borderRadius: 6, color: "var(--green)", fontSize: 11, fontWeight: 700, letterSpacing: 1, cursor: busy ? "wait" : none ? "default" : "pointer", opacity: none ? 0.5 : 1, fontFamily: "monospace" }}
      >
        {busy ? "SCHEDULING…" : "SCHEDULE"}
      </button>
    </form>
  );
}
