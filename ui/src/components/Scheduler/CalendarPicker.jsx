/* FluxDM — Scheduler/CalendarPicker.jsx */
import React from "react";
export default function CalendarPicker({ value, onChange }) {
  return <input type="datetime-local" value={value} onChange={e=>onChange(e.target.value)}
    style={{background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,padding:"8px 12px",color:"#fff",fontFamily:"monospace",fontSize:11,outline:"none"}}/>;
}
