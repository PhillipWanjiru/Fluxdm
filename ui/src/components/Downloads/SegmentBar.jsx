/* FluxDM — Downloads/SegmentBar.jsx */
import React from "react";
export default function SegmentBar({ pct=0, segments=8, status="downloading" }) {
  const filled = Math.round((pct/100)*segments);
  const color = status==="complete"?"var(--blue)":status==="paused"?"#555":"var(--green)";
  return (
    <div style={{display:"flex",gap:2,margin:"6px 0 2px"}}>
      {Array.from({length:segments}).map((_,i)=>(
        <div key={i} style={{height:4,flex:1,borderRadius:2,
          background:i<filled?color:"var(--border)",transition:"background 0.3s"}}/>
      ))}
    </div>
  );
}
