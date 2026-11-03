/* FluxDM — Settings/NetworkSettings.jsx */
import React from "react";
import { useSettings } from "../../hooks/useSettings";
import { SEGMENT_CHOICES } from "../../model/destination";
export default function NetworkSettings() {
  const { settings, update } = useSettings();
  return (
    <div style={{display:"flex",flexDirection:"column",gap:16,maxWidth:400}}>
      <div>
        <label style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>GLOBAL BANDWIDTH LIMIT (KB/s, 0=unlimited)</label>
        <input type="number" value={settings.globalLimit} onChange={e=>update({globalLimit:+e.target.value})}
          style={{width:"100%",background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,padding:"9px 12px",color:"#fff",fontFamily:"monospace",fontSize:12,outline:"none"}}/>
      </div>
      <div>
        <span style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>DEFAULT SEGMENTS</span>
        <div role="group" aria-label="Default segments" style={{display:"flex",gap:4}}>
          {SEGMENT_CHOICES.map(n=>(
            <button key={n} type="button" onClick={()=>update({defaultSegments:n})} aria-pressed={settings.defaultSegments===n} style={{flex:1,padding:"8px 0",background:settings.defaultSegments===n?"rgba(74,244,138,0.1)":"var(--bg-panel)",border:`1px solid ${settings.defaultSegments===n?"var(--green)":"var(--border)"}`,borderRadius:6,color:settings.defaultSegments===n?"var(--green)":"var(--text-dim)",cursor:"pointer",fontFamily:"monospace",fontSize:11}}>{n}×</button>
          ))}
        </div>
      </div>
    </div>
  );
}
