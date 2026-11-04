/* FluxDM — Downloads/SegmentBar.jsx
 * One small bar per segment, filled in proportion to the download's progress. The daemon reports the
 * aggregate only, so this is the overall progress drawn in segment-sized steps, not each segment's own. */
import React from "react";
import { State } from "../../model/downloads";
const COLORS = { [State.COMPLETE]:"var(--blue)", [State.PAUSED]:"#555", [State.QUEUED]:"#555", [State.FAILED]:"var(--red)" };
export default function SegmentBar({ pct=0, segments=8, state=State.DOWNLOADING, label="Progress" }) {
  const count = Math.max(1, segments || 8);
  const filled = Math.round((pct/100)*count);
  const color = COLORS[state] || "var(--green)";
  return (
    <div role="progressbar" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={pct} style={{display:"flex",gap:2,margin:"6px 0 2px"}}>
      {Array.from({length:count}).map((_,i)=>(
        <div key={i} style={{height:4,flex:1,borderRadius:2,
          background:i<filled?color:"var(--border)",transition:"background 0.3s"}}/>
      ))}
    </div>
  );
}
