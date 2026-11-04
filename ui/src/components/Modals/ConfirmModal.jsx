/* FluxDM — Modals/ConfirmModal.jsx
 * Asks before something that cannot be undone. Cancel has the focus when it opens, so Enter or a stray space bar
 * does not destroy anything, and Escape cancels. */
import React, { useEffect } from "react";

export default function ConfirmModal({ title = "Are you sure?", message, confirmLabel = "Confirm", onConfirm, onCancel }) {
  useEffect(() => {
    const onKey = (e) => { if (e.key === "Escape") onCancel(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onCancel]);

  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <div role="alertdialog" aria-modal="true" aria-labelledby="confirm-title" aria-describedby="confirm-message"
        style={{background:"#111",border:"1px solid var(--border)",borderRadius:10,padding:24,width:380,fontFamily:"monospace"}}>
        <div id="confirm-title" style={{color:"#fff",fontSize:13,fontWeight:700,letterSpacing:1,marginBottom:10}}>{title}</div>
        <div id="confirm-message" style={{color:"var(--text-main)",marginBottom:20,fontSize:12,lineHeight:1.5,wordBreak:"break-word"}}>{message}</div>
        <div style={{display:"flex",gap:10}}>
          <button type="button" autoFocus onClick={onCancel} style={{flex:1,padding:10,background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,color:"var(--text-dim)",cursor:"pointer",fontFamily:"inherit"}}>Cancel</button>
          <button type="button" onClick={onConfirm} style={{flex:1,padding:10,background:"rgba(244,74,74,0.1)",border:"1px solid var(--red)",borderRadius:6,color:"var(--red)",cursor:"pointer",fontFamily:"inherit"}}>{confirmLabel}</button>
        </div>
      </div>
    </div>
  );
}
