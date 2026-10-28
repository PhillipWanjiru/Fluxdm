/* FluxDM — Shared/Tooltip.jsx */
import React, { useState } from "react";
export default function Tooltip({ children, text }) {
  const [vis,setVis]=useState(false);
  return (
    <div style={{position:"relative",display:"inline-block"}} onMouseEnter={()=>setVis(true)} onMouseLeave={()=>setVis(false)}>
      {children}
      {vis&&<div style={{position:"absolute",bottom:"100%",left:"50%",transform:"translateX(-50%)",background:"#111",border:"1px solid var(--border)",borderRadius:4,padding:"4px 8px",fontSize:10,whiteSpace:"nowrap",color:"#ccc",zIndex:100}}>{text}</div>}
    </div>
  );
}
