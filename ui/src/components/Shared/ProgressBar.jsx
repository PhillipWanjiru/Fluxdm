/* FluxDM — Shared/ProgressBar.jsx */
import React from "react";
export default function ProgressBar({ pct=0, color="var(--green)" }) {
  return (
    <div style={{height:3,background:"var(--border)",borderRadius:2}}>
      <div style={{height:"100%",width:`${pct}%`,background:color,borderRadius:2,transition:"width 0.3s"}}/>
    </div>
  );
}
