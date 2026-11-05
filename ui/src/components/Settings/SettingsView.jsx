/* FluxDM — Settings/SettingsView.jsx */
import React, { useState } from "react";
import NetworkSettings    from "./NetworkSettings";
import StorageSettings    from "./StorageSettings";
import AppearanceSettings from "./AppearanceSettings";
const TABS = [["network","Network"],["storage","Storage"],["appearance","Appearance"]];
export default function SettingsView() {
  const [tab,setTab] = useState("network");
  return (
    <div style={{padding:24,height:"100%",overflowY:"auto"}}>
      <h2 style={{fontSize:14,fontWeight:700,letterSpacing:1,marginBottom:20,color:"var(--green)"}}>SETTINGS</h2>
      <div style={{display:"flex",gap:2,marginBottom:24,borderBottom:"1px solid var(--border)",paddingBottom:0}}>
        {TABS.map(([id,label])=>(
          <button key={id} type="button" onClick={()=>setTab(id)} aria-pressed={tab===id} style={{background:"none",border:"none",padding:"8px 16px",color:tab===id?"#fff":"var(--text-dim)",fontSize:11,cursor:"pointer",fontFamily:"monospace",borderBottom:`2px solid ${tab===id?"var(--green)":"transparent"}`}}>{label}</button>
        ))}
      </div>
      {tab==="network"    &&<NetworkSettings/>}
      {tab==="storage"    &&<StorageSettings/>}
      {tab==="appearance" &&<AppearanceSettings/>}
    </div>
  );
}
