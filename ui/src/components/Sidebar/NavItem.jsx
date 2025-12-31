/* FluxDM — Sidebar/NavItem.jsx */
import React from "react";
export default function NavItem({ id, label, active, onClick }) {
  return (
    <button onClick={onClick} style={{
      width:"100%",display:"flex",alignItems:"center",padding:"9px 10px",
      background:active?"rgba(74,244,138,0.07)":"none",
      border:`1px solid ${active?"rgba(74,244,138,0.3)":"transparent"}`,
      borderRadius:6,color:active?"var(--green)":"var(--text-dim)",
      fontSize:11,cursor:"pointer",fontFamily:"inherit",textAlign:"left",marginBottom:2
    }}>{label}</button>
  );
}
