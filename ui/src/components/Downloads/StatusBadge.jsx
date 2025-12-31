/* FluxDM — Downloads/StatusBadge.jsx */
import React from "react";
const MAP = {
  downloading: {label:"ACTIVE",color:"#4af48a",bg:"#0a1f12"},
  complete:    {label:"DONE",  color:"#4ab3f4",bg:"#0a1220"},
  queued:      {label:"QUEUE", color:"#f4c44a",bg:"#1f1a0a"},
  paused:      {label:"PAUSED",color:"#888",   bg:"#1a1a1a"},
};
export default function StatusBadge({ status="queued" }) {
  const s = MAP[status] || MAP.queued;
  return (
    <span style={{fontSize:9,fontWeight:700,color:s.color,background:s.bg,
      padding:"3px 7px",borderRadius:4,letterSpacing:1,
      border:`1px solid ${s.color}33`,fontFamily:"monospace"}}>
      {s.label}
    </span>
  );
}
