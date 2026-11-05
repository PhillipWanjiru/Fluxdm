/* FluxDM — Sidebar/BandwidthMeter.jsx
 * How fast the running downloads are going together, and against what: the limit on all downloads together, when
 * there is one (read from the daemon, store/bandwidthSlice.js). Without a limit there is nothing to measure the
 * speed against, so there is no bar, only the speed. */
import React from "react";
import { useSelector } from "react-redux";
import { formatSpeed } from "../../model/format";
import { selectLimitBps } from "../../store/bandwidthSlice";
import { selectTotalSpeed } from "../../store/downloadsSlice";

export default function BandwidthMeter() {
  const speed = useSelector(selectTotalSpeed);
  const limit = useSelector(selectLimitBps);
  const limited = limit !== null && limit > 0;
  const used = limited ? Math.min(speed, limit) : 0; /* a burst above the limit is shown as the limit */

  return (
    <div style={{padding:"12px 14px",borderTop:"1px solid var(--border)"}}>
      <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:6}}>BANDWIDTH</div>
      <div style={{fontSize:10,color:"var(--green)",marginBottom:limited?3:0}}>
        ↓ {formatSpeed(speed)}
        {limited && <span style={{color:"var(--text-dim)"}}>{` of ${formatSpeed(limit)}`}</span>}
      </div>
      {limited && (
        <div role="meter" aria-label="Bandwidth in use" aria-valuemin={0} aria-valuemax={limit} aria-valuenow={used}
          aria-valuetext={`${formatSpeed(speed)} of ${formatSpeed(limit)}`}
          style={{height:3,background:"var(--border)",borderRadius:2}}>
          <div data-testid="bandwidth-fill" style={{height:"100%",width:`${(used/limit)*100}%`,background:"var(--green)",borderRadius:2,transition:"width 0.5s"}} />
        </div>
      )}
      {limit === 0 && <div style={{fontSize:9,color:"var(--text-dim)",marginTop:3}}>No limit</div>}
    </div>
  );
}
