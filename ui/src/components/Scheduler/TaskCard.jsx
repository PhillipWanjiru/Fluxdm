/* FluxDM — Scheduler/TaskCard.jsx */
import React from "react";
export default function TaskCard({ task, onDelete }) {
  return (
    <div style={{background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:8,padding:"12px 16px",marginBottom:8,display:"flex",justifyContent:"space-between",alignItems:"center"}}>
      <div>
        <div style={{fontSize:11,color:"#ccc"}}>{task.url||"—"}</div>
        <div style={{fontSize:9,color:"var(--text-dim)",marginTop:4}}>Trigger: {new Date(task.trigger_at*1000).toLocaleString()}</div>
      </div>
      <button onClick={onDelete} style={{background:"none",border:"1px solid var(--red)",borderRadius:4,color:"var(--red)",padding:"4px 10px",cursor:"pointer",fontFamily:"monospace",fontSize:10}}>DELETE</button>
    </div>
  );
}
