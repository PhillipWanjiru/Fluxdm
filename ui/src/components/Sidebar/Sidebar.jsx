/* FluxDM — Sidebar/Sidebar.jsx */
import React from "react";
import NavItem from "./NavItem";
const NAV = [["downloads","Downloads"],["grabber","Video Grabber"],["scheduler","Scheduler"],["settings","Settings"]];
export default function Sidebar({ active, onNavigate }) {
  return (
    <div style={{width:200,background:"var(--bg-panel)",borderRight:"1px solid var(--border)",display:"flex",flexDirection:"column"}}>
      <div style={{padding:"20px 16px",borderBottom:"1px solid var(--border)"}}>
        <div style={{fontSize:18,fontWeight:700,letterSpacing:2,color:"var(--green)"}}>FLUX<span style={{color:"#fff"}}>DM</span></div>
      </div>
      <div style={{padding:"12px 8px",flex:1}}>
        {NAV.map(([id,label])=><NavItem key={id} id={id} label={label} active={active===id} onClick={()=>onNavigate(id)} />)}
      </div>
    </div>
  );
}
