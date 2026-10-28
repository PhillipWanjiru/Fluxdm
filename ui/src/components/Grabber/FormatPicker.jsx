/* FluxDM — Grabber/FormatPicker.jsx */
import React from "react";
export default function FormatPicker({ formats=[], selected, onSelect }) {
  return (
    <div style={{display:"flex",flexDirection:"column",gap:4}}>
      {formats.map(f=>(
        <div key={f.id} onClick={()=>onSelect(f.id)} style={{padding:"8px 12px",borderRadius:6,cursor:"pointer",
          background:selected===f.id?"rgba(74,244,138,0.1)":"var(--bg-hover)",
          border:`1px solid ${selected===f.id?"var(--green)":"var(--border)"}`,fontSize:11}}>
          {f.ext} {f.height?`${f.height}p`:""} {f.abr?`${f.abr}kbps`:""}
        </div>
      ))}
    </div>
  );
}
