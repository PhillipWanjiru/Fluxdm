/* FluxDM — Grabber/VideoGrabber.jsx */
import React, { useState } from "react";
import { detect, grab } from "../../api/grabber";
import GrabModal from "../Modals/GrabModal";
export default function VideoGrabber() {
  const [url,setUrl]       = useState("");
  const [meta,setMeta]     = useState(null);
  const [loading,setLoad]  = useState(false);
  const [modal,setModal]   = useState(false);
  const handleDetect = async () => {
    if(!url) return; setLoad(true);
    try { const r=await detect(url); setMeta(r.data); setModal(true); } finally { setLoad(false); }
  };
  return (
    <div style={{padding:24}}>
      <div style={{fontSize:14,fontWeight:700,letterSpacing:1,marginBottom:20,color:"var(--green)"}}>VIDEO GRABBER</div>
      <div style={{display:"flex",gap:10,marginBottom:20}}>
        <input value={url} onChange={e=>setUrl(e.target.value)} placeholder="Paste video URL..." style={{flex:1,background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,padding:"10px 14px",color:"#fff",fontSize:12,fontFamily:"monospace",outline:"none"}}/>
        <button onClick={handleDetect} disabled={loading} style={{padding:"10px 20px",background:"var(--green)",border:"none",borderRadius:6,color:"#000",fontWeight:700,cursor:"pointer",fontFamily:"monospace"}}>
          {loading?"...":"DETECT"}
        </button>
      </div>
      {modal&&meta&&<GrabModal formats={meta.formats||[]} onGrab={async(fid)=>{await grab(url,fid);setModal(false);}} onClose={()=>setModal(false)}/>}
    </div>
  );
}
