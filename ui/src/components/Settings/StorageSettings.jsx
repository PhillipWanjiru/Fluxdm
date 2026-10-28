/* FluxDM — Settings/StorageSettings.jsx */
import React from "react";
export default function StorageSettings() {
  return (
    <div style={{display:"flex",flexDirection:"column",gap:16,maxWidth:400}}>
      <div>
        <label style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>DEFAULT DOWNLOAD PATH</label>
        <input defaultValue="~/Downloads" style={{width:"100%",background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,padding:"9px 12px",color:"#fff",fontFamily:"monospace",fontSize:12,outline:"none",boxSizing:"border-box"}}/>
      </div>
    </div>
  );
}
