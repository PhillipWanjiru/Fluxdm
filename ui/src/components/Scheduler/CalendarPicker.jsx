/* FluxDM — Scheduler/CalendarPicker.jsx
 * A date and time in the user's own time zone, with the browser's native picker. `value` and `onChange` use the
 * input's own text ("2026-11-06T02:00"); model/tasks.js turns that into the epoch seconds the daemon wants. */
import React from "react";

export default function CalendarPicker({ id, value, onChange, min, invalid = false, describedBy }) {
  return (
    <input
      id={id}
      type="datetime-local"
      value={value}
      min={min}
      onChange={(e) => onChange(e.target.value)}
      aria-invalid={invalid}
      aria-describedby={describedBy}
      style={{
        width: "100%",
        background: "#0a0a0a",
        border: `1px solid ${invalid ? "var(--red)" : "var(--border)"}`,
        borderRadius: 6,
        padding: "9px 12px",
        color: "#fff",
        fontFamily: "monospace",
        fontSize: 12,
        outline: "none",
        colorScheme: "dark" /* the picker's own icon and pop-up, light on dark */,
      }}
    />
  );
}
