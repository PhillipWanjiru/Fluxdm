/* FluxDM — Downloads/StatusBadge.jsx */
import React from "react";
import { State } from "../../model/downloads";
const MAP = {
  [State.DOWNLOADING]: {label:"ACTIVE",color:"#4af48a",bg:"#0a1f12"},
  [State.COMPLETE]:    {label:"DONE",  color:"#4ab3f4",bg:"#0a1220"},
  [State.QUEUED]:      {label:"QUEUE", color:"#f4c44a",bg:"#1f1a0a"},
  [State.PAUSED]:      {label:"PAUSED",color:"#888",   bg:"#1a1a1a"},
  [State.FAILED]:      {label:"FAILED",color:"#f44a4a",bg:"#1f0a0a"},
};
export default function StatusBadge({ state=State.QUEUED }) {
  const s = MAP[state] || MAP[State.QUEUED];
  return (
    <span style={{fontSize:9,fontWeight:700,color:s.color,background:s.bg,
      padding:"3px 7px",borderRadius:4,letterSpacing:1,
      border:`1px solid ${s.color}33`,fontFamily:"monospace"}}>
      {s.label}
    </span>
  );
}
