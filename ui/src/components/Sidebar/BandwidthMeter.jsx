/* FluxDM — BandwidthMeter.jsx */
import React from "react";
export default function BandwidthMeter({ speed = 0, max = 20 }) {
  const pct = Math.min(100, (speed/max)*100);
  return (
    <div style={{padding:"12px 14px",borderTop:"1px solid var(--border)"}}>
      <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:6}}>BANDWIDTH</div>
      <div style={{fontSize:10,color:"var(--green)",marginBottom:3}}>↓ {speed.toFixed(1)} MB/s</div>
      <div style={{height:3,background:"var(--border)",borderRadius:2}}>
        <div style={{height:"100%",width:`${pct}%`,background:"var(--green)",borderRadius:2,transition:"width 0.5s"}} />
      </div>
    </div>
  );
}
