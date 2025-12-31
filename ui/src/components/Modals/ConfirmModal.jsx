/* FluxDM — Modals/ConfirmModal.jsx */
import React from "react";
export default function ConfirmModal({ message, onConfirm, onCancel }) {
  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <div style={{background:"#111",border:"1px solid var(--border)",borderRadius:10,padding:24,width:360,fontFamily:"monospace"}}>
        <div style={{color:"#fff",marginBottom:20,fontSize:13}}>{message}</div>
        <div style={{display:"flex",gap:10}}>
          <button onClick={onCancel}  style={{flex:1,padding:10,background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,color:"var(--text-dim)",cursor:"pointer",fontFamily:"inherit"}}>Cancel</button>
          <button onClick={onConfirm} style={{flex:1,padding:10,background:"rgba(244,74,74,0.1)",border:"1px solid var(--red)",borderRadius:6,color:"var(--red)",cursor:"pointer",fontFamily:"inherit"}}>Confirm</button>
        </div>
      </div>
    </div>
  );
}
