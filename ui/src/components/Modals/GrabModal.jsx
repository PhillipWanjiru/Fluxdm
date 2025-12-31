/* FluxDM — Modals/GrabModal.jsx */
import React, { useState } from "react";
export default function GrabModal({ formats=[], onGrab, onClose }) {
  const [selected, setSelected] = useState(formats[0]?.id||"best");
  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <div style={{background:"#111",border:"1px solid var(--border)",borderRadius:10,padding:24,width:420,fontFamily:"monospace"}}>
        <div style={{color:"#fff",fontSize:14,fontWeight:700,marginBottom:16}}>SELECT FORMAT</div>
        <div style={{maxHeight:200,overflowY:"auto",marginBottom:16}}>
          {formats.map(f=>(
            <div key={f.id} onClick={()=>setSelected(f.id)} style={{padding:"8px 12px",marginBottom:4,background:selected===f.id?"rgba(74,244,138,0.1)":"var(--bg-hover)",border:`1px solid ${selected===f.id?"var(--green)":"var(--border)"}`,borderRadius:6,cursor:"pointer",fontSize:11}}>
              {f.ext?.toUpperCase()} {f.height?`${f.height}p`:""} {f.abr?`${f.abr}k`:""}
            </div>
          ))}
        </div>
        <div style={{display:"flex",gap:10}}>
          <button onClick={onClose}         style={{flex:1,padding:10,background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,color:"var(--text-dim)",cursor:"pointer",fontFamily:"inherit"}}>Cancel</button>
          <button onClick={()=>onGrab(selected)} style={{flex:1,padding:10,background:"rgba(74,244,138,0.1)",border:"1px solid var(--green)",borderRadius:6,color:"var(--green)",cursor:"pointer",fontFamily:"inherit"}}>Download</button>
        </div>
      </div>
    </div>
  );
}
