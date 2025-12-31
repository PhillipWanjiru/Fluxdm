/* FluxDM — Modals/AddDownloadModal.jsx */
import React, { useState } from "react";
import { start } from "../../api/downloads";
export default function AddDownloadModal({ onClose }) {
  const [url,setUrl]   = useState("");
  const [segs,setSegs] = useState(8);
  const submit = async () => {
    if(!url) return;
    await start({url,segments:segs});
    onClose();
  };
  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <div style={{background:"#111",border:"1px solid var(--border)",borderRadius:12,padding:28,width:520,fontFamily:"monospace"}}>
        <div style={{display:"flex",justifyContent:"space-between",alignItems:"center",marginBottom:20}}>
          <span style={{color:"#fff",fontSize:14,fontWeight:700,letterSpacing:1}}>NEW DOWNLOAD</span>
          <button onClick={onClose} style={{background:"none",border:"none",color:"var(--text-dim)",fontSize:18,cursor:"pointer"}}>×</button>
        </div>
        <label style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>URL</label>
        <input value={url} onChange={e=>setUrl(e.target.value)} placeholder="https://" style={{width:"100%",background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,padding:"10px 12px",color:"#fff",fontSize:12,fontFamily:"inherit",boxSizing:"border-box",outline:"none",marginBottom:14}}/>
        <label style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>SEGMENTS</label>
        <div style={{display:"flex",gap:4,marginBottom:20}}>
          {[4,8,16,32].map(n=>(
            <button key={n} onClick={()=>setSegs(n)} style={{flex:1,padding:"8px 0",background:segs===n?"rgba(74,244,138,0.1)":"#0a0a0a",border:`1px solid ${segs===n?"var(--green)":"var(--border)"}`,borderRadius:6,color:segs===n?"var(--green)":"var(--text-dim)",fontSize:12,cursor:"pointer",fontFamily:"inherit"}}>{n}×</button>
          ))}
        </div>
        <div style={{display:"flex",gap:10}}>
          <button onClick={onClose} style={{flex:1,padding:11,background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,color:"var(--text-dim)",fontSize:12,cursor:"pointer",fontFamily:"inherit"}}>CANCEL</button>
          <button onClick={submit} style={{flex:2,padding:11,background:"rgba(74,244,138,0.1)",border:"1px solid var(--green)",borderRadius:6,color:"var(--green)",fontSize:12,fontWeight:700,cursor:"pointer",fontFamily:"inherit",letterSpacing:1}}>START DOWNLOAD</button>
        </div>
      </div>
    </div>
  );
}
