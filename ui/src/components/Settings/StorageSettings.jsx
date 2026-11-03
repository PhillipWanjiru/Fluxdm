/* FluxDM — Settings/StorageSettings.jsx */
import React from "react";
import { useSettings } from "../../hooks/useSettings";
import { hasControlChars, isAbsoluteDirectory } from "../../model/destination";

export default function StorageSettings() {
  const { settings, update } = useSettings();
  /* The same test store/persist.js applies, so what is flagged here is exactly what would not be kept. */
  const bad = !isAbsoluteDirectory(settings.downloadDir) || hasControlChars(settings.downloadDir);
  return (
    <div style={{display:"flex",flexDirection:"column",gap:16,maxWidth:400}}>
      <div>
        <label htmlFor="default-dir" style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>DEFAULT DOWNLOAD FOLDER</label>
        <input id="default-dir" value={settings.downloadDir} onChange={e=>update({downloadDir:e.target.value})} spellCheck={false}
          aria-invalid={bad} aria-describedby="default-dir-help"
          style={{width:"100%",background:"var(--bg-panel)",border:`1px solid ${bad?"var(--red)":"var(--border)"}`,borderRadius:6,padding:"9px 12px",color:"#fff",fontFamily:"monospace",fontSize:12,outline:"none",boxSizing:"border-box"}}/>
        <div id="default-dir-help" style={{fontSize:9,color:bad?"var(--red)":"var(--text-dim)",marginTop:6}}>
          {bad
            ? "Not saved: use an absolute path, or one that starts with ~/."
            : "An absolute path, or one that starts with ~/. New downloads are offered this folder."}
        </div>
      </div>
    </div>
  );
}
